//! The loopback-only PoseBridge UDP receiver (formerly C++ `OscHeadTrackingReceiver`).
//!
//! A background thread receives datagrams on an IPv4 loopback socket, decodes them with
//! [`decode`](crate::decode) and replaces a single latest snapshot under a mutex. No audio
//! device, GUI, BLE dependency or callback: a host polls [`Receiver::snapshot`] on a control
//! thread and applies recentering, smoothing and arbitration itself. The rules, counters,
//! ordering and texts are the C++ receiver's.
//!
//! `start` and `stop` are serialized by the receiver; `snapshot`, `snapshot_json` and
//! `last_error` may run alongside them. `stop` joins the worker, so it is not an
//! audio-callback call.

use crate::{Kind, Message, PROTOCOL, SourceOrder, Timing, decode, valid_source_id};
use mradm_json::{Map, Value};
use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

const STALE_AFTER: Duration = Duration::from_millis(500);
const HEARTBEAT_ALIVE: Duration = Duration::from_secs(3);
/// The worker's wake-up period while waiting for a datagram, and so the latency of `stop`.
const POLL: Duration = Duration::from_millis(20);
/// One latest message per kind and source instance.
const TELEMETRY_INSTANCES: usize = 17;
/// A full UDP datagram: an oversized valid prefix must not be accepted.
const DATAGRAM: usize = 65536;

/// `adm::ErrorCode` values the receiver returns.
pub const INVALID_ARGUMENT: i32 = 1;
pub const IO_ERROR: i32 = 3;
pub const INTERNAL_ERROR: i32 = 6;

static NEXT_SESSION: AtomicU64 = AtomicU64::new(1);

/// `OscHeadTrackingState`; the numeric values are the C ABI's.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum State {
    #[default]
    Idle = 0,
    Waiting = 1,
    Active = 2,
    Stale = 3,
    Stopped = 4,
    Failed = 5,
}

/// `OscHeadTrackingSnapshot`.
#[derive(Clone, Debug, PartialEq)]
pub struct Snapshot {
    pub state: State,
    pub bound_port: u16,
    pub has_pose: bool,
    pub fresh: bool,
    pub session_id: u64,
    pub sequence: u64,
    /// Monotonic time since this start; not device sampling time.
    pub received_ns: u64,
    /// Meaningful only when `has_pose`.
    pub age_ms: u64,
    pub packets_received: u64,
    pub rejected_packets: u64,
    /// Valid input after at least 500 ms of silence; the caller should check recentering.
    pub recovery_count: u64,
    pub pose_packets: u64,
    pub telemetry_packets: u64,
    pub ignored_sources: u64,
    pub protocol_mismatches: u64,
    pub missing_tx_packets: u64,
    pub has_heartbeat: bool,
    pub heartbeat_alive: bool,
    pub heartbeat_age_ms: u64,
    /// Bound by the first accepted pose, or specified at construction.
    pub source_id: String,
    pub info_json: String,
    pub status_json: String,
    pub info_matches_pose: bool,
    pub status_matches_pose: bool,
    pub quaternion_xyzw: [f32; 4],
    /// Yaw, pitch, roll in degrees.
    pub euler_deg: [f32; 3],
    pub timing: Timing,
}

impl Default for Snapshot {
    fn default() -> Self {
        Self {
            state: State::Idle,
            bound_port: 0,
            has_pose: false,
            fresh: false,
            session_id: 0,
            sequence: 0,
            received_ns: 0,
            age_ms: 0,
            packets_received: 0,
            rejected_packets: 0,
            recovery_count: 0,
            pose_packets: 0,
            telemetry_packets: 0,
            ignored_sources: 0,
            protocol_mismatches: 0,
            missing_tx_packets: 0,
            has_heartbeat: false,
            heartbeat_alive: false,
            heartbeat_age_ms: 0,
            source_id: String::new(),
            info_json: String::new(),
            status_json: String::new(),
            info_matches_pose: false,
            status_matches_pose: false,
            quaternion_xyzw: [0., 0., 0., 1.],
            euler_deg: [0.; 3],
            timing: Timing::default(),
        }
    }
}

/// A failed start: the `adm::ErrorCode` and the message.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StartError {
    pub code: i32,
    pub message: String,
}

/// A bind failure: the C++ step text and the OS error code.
#[derive(Debug)]
pub struct BindError {
    pub message: &'static str,
    pub code: i32,
}

impl BindError {
    pub fn new(message: &'static str, error: &io::Error) -> Self {
        Self {
            message,
            code: error.raw_os_error().unwrap_or(0),
        }
    }
}

/// A socket bound to 127.0.0.1, with whatever platform state must outlive it (`guard` is
/// dropped after the socket: fields drop in declaration order).
pub struct Bound {
    pub socket: UdpSocket,
    pub guard: Option<Box<dyn Send + Sync>>,
}

/// Binds the loopback socket for `port` (0 lets the OS choose). Never chooses another port.
pub type Bind = fn(u16) -> Result<Bound, BindError>;

/// The portable bind: `std::net` creates the socket close-on-exec and without address reuse.
pub fn bind_loopback(port: u16) -> Result<Bound, BindError> {
    let socket = UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, port)).map_err(|e| {
        let message = match e.kind() {
            io::ErrorKind::AddrInUse
            | io::ErrorKind::AddrNotAvailable
            | io::ErrorKind::PermissionDenied => "OSC 回环端口绑定失败，端口可能已被占用",
            _ => "OSC UDP socket 创建失败",
        };
        BindError::new(message, &e)
    })?;
    Ok(Bound {
        socket,
        guard: None,
    })
}

struct Cached {
    message: Message,
    json: String,
    received_at: Instant,
}

struct Inner {
    state: Snapshot,
    source_order: SourceOrder,
    started_at: Instant,
    received_at: Instant,
    heartbeat_at: Instant,
    source_allows_pose: bool,
    info_cache: Vec<Cached>,
    status_cache: Vec<Cached>,
    error_message: String,
    socket_open: bool,
}

impl Inner {
    fn new() -> Self {
        let now = Instant::now();
        Self {
            state: Snapshot::default(),
            source_order: SourceOrder::default(),
            started_at: now,
            received_at: now,
            heartbeat_at: now,
            source_allows_pose: true,
            info_cache: Vec::new(),
            status_cache: Vec::new(),
            error_message: String::new(),
            socket_open: false,
        }
    }

    fn fail(&mut self, message: &str, code: i32) -> String {
        self.state.state = State::Failed;
        if !self.socket_open {
            self.state.bound_port = 0;
        }
        self.error_message = format!("{message} ({code})");
        self.error_message.clone()
    }

    fn matches_current_instance(&self, cached: &Cached) -> bool {
        self.state.has_pose
            && cached.message.source_id == self.state.source_id
            && cached.message.timing.instance_id == self.state.timing.instance_id
    }

    fn current_telemetry<'a>(&self, cache: &'a [Cached]) -> Option<&'a Cached> {
        if !self.state.has_pose {
            return cache.last();
        }
        cache.iter().find(|c| self.matches_current_instance(c))
    }

    fn update_metadata(&mut self) {
        let info = self.current_telemetry(&self.info_cache);
        let status = self.current_telemetry(&self.status_cache);
        let state = &self.state;
        let current = state.timing;
        let matches_pose = |t: &Timing| {
            state.has_pose
                && t.instance_id == current.instance_id
                && t.source_session_id == current.source_session_id
                && t.metadata_revision == current.metadata_revision
                && t.reference_epoch == current.reference_epoch
        };
        let changes_reference = |t: &Timing| {
            state.has_pose
                && t.metadata_revision >= current.metadata_revision
                && (t.reference_epoch > current.reference_epoch
                    || t.source_session_id != current.source_session_id)
        };
        let info_json = info.map_or_else(String::new, |c| c.json.clone());
        let status_json = status.map_or_else(String::new, |c| c.json.clone());
        let info_matches_pose = info.is_some_and(|c| matches_pose(&c.message.timing));
        let status_matches_pose = status.is_some_and(|c| matches_pose(&c.message.timing));
        let mut changed_reference = info.is_some_and(|c| changes_reference(&c.message.timing));
        let mut allows = None;
        let mut heartbeat_at = None;
        if let Some(c) = status {
            let message = &c.message;
            changed_reference = changed_reference || changes_reference(&message.timing);
            heartbeat_at = Some(c.received_at);
            if state.has_pose && message.timing.metadata_revision >= current.metadata_revision {
                if message.timing.source_session_id == current.source_session_id {
                    if message.reported_samples >= current.source_sequence {
                        allows = Some(message.source_active);
                    }
                } else {
                    allows = Some(false);
                }
            }
        }
        let has_heartbeat = status.is_some();
        self.state.info_json = info_json;
        self.state.status_json = status_json;
        self.state.info_matches_pose = info_matches_pose;
        self.state.status_matches_pose = status_matches_pose;
        self.state.has_heartbeat = has_heartbeat;
        if let Some(at) = heartbeat_at {
            self.heartbeat_at = at;
        }
        if let Some(value) = allows {
            self.source_allows_pose = value;
        }
        if changed_reference {
            self.source_allows_pose = false;
        }
    }

    fn accept_telemetry(&mut self, message: Message, json: String, now: Instant) {
        let t = message.timing;
        let info = message.kind == Kind::Info;
        let cache = if info {
            &self.info_cache
        } else {
            &self.status_cache
        };
        let cached = cache.iter().position(|c| {
            c.message.source_id == message.source_id
                && c.message.timing.instance_id == t.instance_id
        });
        let state = &self.state;
        let old = self.source_order.retired(t.instance_id)
            || cached.is_some_and(|i| {
                let c = &cache[i].message;
                message.message_sequence <= c.message_sequence
                    || t.metadata_revision < c.timing.metadata_revision
                    || t.reference_epoch < c.timing.reference_epoch
            })
            || (state.has_pose
                && t.instance_id == state.timing.instance_id
                && (t.metadata_revision < state.timing.metadata_revision
                    || t.reference_epoch < state.timing.reference_epoch
                    || (t.source_session_id != state.timing.source_session_id
                        && t.metadata_revision <= state.timing.metadata_revision)));
        if old {
            self.state.rejected_packets += 1;
            return;
        }
        let evict = match cached {
            Some(i) => Some(i),
            // The C++ receiver erased its first candidate; one always exists, since at most
            // one cached entry belongs to the current pose instance.
            None if cache.len() >= TELEMETRY_INSTANCES => {
                cache.iter().position(|c| !self.matches_current_instance(c))
            }
            None => None,
        };
        let cache = if info {
            &mut self.info_cache
        } else {
            &mut self.status_cache
        };
        if let Some(i) = evict {
            cache.remove(i);
        }
        cache.push(Cached {
            message,
            json,
            received_at: now,
        });
        self.state.telemetry_packets += 1;
        self.update_metadata();
    }

    fn accept_message(
        &mut self,
        requested_source: &[u8],
        message: Message,
        json: String,
        now: Instant,
    ) {
        if message.kind == Kind::Incompatible {
            self.state.rejected_packets += 1;
            self.state.protocol_mismatches += 1;
            self.error_message =
                "PoseBridge protocol mismatch: receiver requires protocol 3".into();
            return;
        }
        if (!requested_source.is_empty() && message.source_id.as_bytes() != requested_source)
            || (self.state.has_pose && message.source_id != self.state.source_id)
        {
            self.state.ignored_sources += 1;
            return;
        }
        if message.kind != Kind::Pose {
            self.accept_telemetry(message, json, now);
            return;
        }
        if !self.source_order.accept(&message.timing) {
            self.state.rejected_packets += 1;
            return;
        }
        if self.state.has_pose && now.duration_since(self.received_at) >= STALE_AFTER {
            self.state.recovery_count += 1;
        }
        let state = &mut self.state;
        state.source_id = message.source_id;
        state.has_pose = true;
        state.quaternion_xyzw = message.quaternion_xyzw;
        state.euler_deg = message.euler_deg;
        state.timing = message.timing;
        self.source_allows_pose = true;
        self.received_at = now;
        state.received_ns = nanos(now.duration_since(self.started_at));
        state.missing_tx_packets = state
            .missing_tx_packets
            .saturating_add(self.source_order.last_gap);
        state.sequence += 1;
        state.pose_packets += 1;
        state.state = State::Active;
        self.update_metadata();
    }
}

/// `std::chrono::duration_cast` of a non-negative steady-clock difference.
fn nanos(d: Duration) -> u64 {
    u64::try_from(d.as_nanos()).unwrap_or(u64::MAX)
}

fn millis(d: Duration) -> u64 {
    u64::try_from(d.as_millis()).unwrap_or(u64::MAX)
}

struct Shared {
    inner: Mutex<Inner>,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Inner> {
        // A panicking worker leaves the plain state usable, as the C++ catch-all did.
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }
}

struct Worker {
    stop: Arc<AtomicBool>,
    thread: JoinHandle<()>,
}

#[derive(Default)]
struct Control {
    worker: Option<Worker>,
    socket: Option<Arc<Bound>>,
}

pub struct Receiver {
    requested_port: u16,
    requested_source: Arc<[u8]>,
    bind: Bind,
    shared: Arc<Shared>,
    control: Mutex<Control>,
}

impl Receiver {
    /// Port 0 requests an OS-assigned port. The source filter (empty for none) is checked by
    /// `start`.
    pub fn new(port: u16, source_id: &[u8], bind: Bind) -> Self {
        Self {
            requested_port: port,
            requested_source: source_id.into(),
            bind,
            shared: Arc::new(Shared {
                inner: Mutex::new(Inner::new()),
            }),
            control: Mutex::new(Control::default()),
        }
    }

    fn control(&self) -> MutexGuard<'_, Control> {
        self.control.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Binds synchronously and starts the worker; idempotent while running.
    pub fn start(&self) -> Result<(), StartError> {
        if !self.requested_source.is_empty() && !valid_source_id(&self.requested_source) {
            return Err(StartError {
                code: INVALID_ARGUMENT,
                message: "source_id must be 1..256 valid UTF-8 bytes without controls".into(),
            });
        }
        let mut control = self.control();
        {
            let inner = self.shared.lock();
            if matches!(inner.state.state, State::Waiting | State::Active) {
                return Ok(());
            }
        }
        self.stop_locked(&mut control);
        {
            let mut inner = self.shared.lock();
            inner.state = Snapshot {
                // Valid UTF-8: checked above.
                source_id: String::from_utf8_lossy(&self.requested_source).into_owned(),
                ..Snapshot::default()
            };
            inner.source_order = SourceOrder::default();
            inner.info_cache.clear();
            inner.status_cache.clear();
            inner.source_allows_pose = true;
            inner.error_message.clear();
        }
        let io_error = |message: String| StartError {
            code: IO_ERROR,
            message,
        };
        // Bind failures leave the receiver reusable and never silently choose another port.
        let bound = match (self.bind)(self.requested_port) {
            Ok(bound) => bound,
            Err(e) => return Err(io_error(self.shared.lock().fail(e.message, e.code))),
        };
        if let Err(e) = bound.socket.set_read_timeout(Some(POLL)) {
            drop(bound);
            return Err(io_error(
                self.shared.lock().fail("OSC UDP socket 配置失败", code(&e)),
            ));
        }
        let port = match bound.socket.local_addr() {
            Ok(address) => address.port(),
            Err(e) => {
                drop(bound);
                return Err(io_error(
                    self.shared.lock().fail("OSC 监听端口查询失败", code(&e)),
                ));
            }
        };
        let bound = Arc::new(bound);
        {
            let mut inner = self.shared.lock();
            inner.socket_open = true;
            inner.started_at = Instant::now();
            inner.state.session_id = NEXT_SESSION.fetch_add(1, Ordering::Relaxed);
            inner.state.bound_port = port;
            inner.state.state = State::Waiting;
        }
        let stop = Arc::new(AtomicBool::new(false));
        let worker = {
            let (stop, shared, bound) = (stop.clone(), self.shared.clone(), bound.clone());
            let source = self.requested_source.clone();
            std::thread::Builder::new()
                .name("mradm-osc".into())
                .spawn(move || receive(&bound.socket, &stop, &shared, &source))
        };
        match worker {
            Ok(thread) => {
                control.worker = Some(Worker { stop, thread });
                control.socket = Some(bound);
                Ok(())
            }
            Err(_) => {
                drop(bound);
                let mut inner = self.shared.lock();
                inner.socket_open = false;
                Err(StartError {
                    code: INTERNAL_ERROR,
                    message: inner.fail("OSC 接收线程创建失败", 0),
                })
            }
        }
    }

    /// Idempotent; keeps the last pose with `fresh` false.
    pub fn stop(&self) {
        let mut control = self.control();
        self.stop_locked(&mut control);
    }

    fn stop_locked(&self, control: &mut Control) {
        if let Some(worker) = control.worker.take() {
            worker.stop.store(true, Ordering::Relaxed);
            let _ = worker.thread.join();
        }
        control.socket = None;
        let mut inner = self.shared.lock();
        inner.socket_open = false;
        inner.state.state = State::Stopped;
        inner.state.bound_port = 0;
    }

    pub fn snapshot(&self) -> Snapshot {
        let inner = self.shared.lock();
        let mut result = inner.state.clone();
        let now = Instant::now();
        if result.has_pose {
            let age = now.duration_since(inner.received_at);
            result.age_ms = millis(age);
            result.fresh = result.state == State::Active
                && inner.source_allows_pose
                && age + Duration::from_nanos(result.timing.source_age_at_send_ns) < STALE_AFTER;
            if result.state == State::Active && !result.fresh {
                result.state = State::Stale;
            }
        }
        if result.has_heartbeat {
            let age = now.duration_since(inner.heartbeat_at);
            result.heartbeat_age_ms = millis(age);
            result.heartbeat_alive =
                matches!(result.state, State::Waiting | State::Active | State::Stale)
                    && age < HEARTBEAT_ALIVE;
        }
        result
    }

    /// The snapshot as `mradm.posebridge` JSON, `nlohmann::json::dump()` bytes.
    pub fn snapshot_json(&self) -> Result<String, String> {
        snapshot_json(&self.snapshot())
    }

    pub fn last_error(&self) -> String {
        let inner = self.shared.lock();
        if inner.state.state == State::Failed && inner.error_message.is_empty() {
            return "OSC 接收线程异常终止".into();
        }
        inner.error_message.clone()
    }
}

impl Drop for Receiver {
    fn drop(&mut self) {
        self.stop();
    }
}

fn code(e: &io::Error) -> i32 {
    e.raw_os_error().unwrap_or(0)
}

fn retryable(e: &io::Error) -> bool {
    matches!(
        e.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
    )
}

fn loopback(peer: &SocketAddr) -> bool {
    match peer {
        SocketAddr::V4(v4) => v4.ip().octets()[0] == 127,
        SocketAddr::V6(_) => false,
    }
}

fn receive(socket: &UdpSocket, stop: &AtomicBool, shared: &Shared, source: &[u8]) {
    let mut buffer = vec![0u8; DATAGRAM];
    while !stop.load(Ordering::Relaxed) {
        let (size, peer) = match socket.recv_from(&mut buffer) {
            Ok(received) => received,
            Err(e) if retryable(&e) => continue,
            Err(e) => {
                shared.lock().fail("OSC UDP 接收失败", code(&e));
                return;
            }
        };
        let now = Instant::now();
        let data = &buffer[..size];
        let message = decode(data).and_then(|message| {
            let json = std::str::from_utf8(data.get(message.json.clone())?)
                .ok()?
                .to_owned();
            Some((message, json))
        });
        let mut inner = shared.lock();
        inner.state.packets_received += 1;
        match message {
            Some((message, json)) if loopback(&peer) => {
                inner.accept_message(source, message, json, now)
            }
            _ => inner.state.rejected_packets += 1,
        }
    }
}

fn decimal(value: u64) -> Value {
    Value::String(value.to_string())
}

fn telemetry(text: &str) -> Result<Value, String> {
    if text.is_empty() {
        return Ok(Value::Null);
    }
    mradm_json::parse(text.as_bytes()).map_err(|e| e.to_string())
}

/// The C++ `snapshot_json`: nlohmann objects keep their keys in byte order, telemetry text is
/// parsed and dumped again, and every 64-bit counter is a decimal string.
pub fn snapshot_json(s: &Snapshot) -> Result<String, String> {
    let t = &s.timing;
    let mut result = Map::new();
    let mut put = |key: &str, value: Value| {
        result.insert(key.to_owned(), value);
    };
    put("schema", PROTOCOL.into());
    put("state", (s.state as i32).into());
    put("bound_port", u32::from(s.bound_port).into());
    put("source_id", s.source_id.as_str().into());
    put("has_pose", s.has_pose.into());
    put("fresh", s.fresh.into());
    put("receiver_session_id", decimal(s.session_id));
    put("receiver_sequence", decimal(s.sequence));
    put("received_ns", decimal(s.received_ns));
    put("age_ms", decimal(s.age_ms));
    put("packets_received", decimal(s.packets_received));
    put("rejected_packets", decimal(s.rejected_packets));
    put("pose_packets", decimal(s.pose_packets));
    put("telemetry_packets", decimal(s.telemetry_packets));
    put("ignored_sources", decimal(s.ignored_sources));
    put("protocol_mismatches", decimal(s.protocol_mismatches));
    put("missing_tx_packets", decimal(s.missing_tx_packets));
    put("recovery_count", decimal(s.recovery_count));
    put("has_heartbeat", s.has_heartbeat.into());
    put("heartbeat_alive", s.heartbeat_alive.into());
    put("heartbeat_age_ms", decimal(s.heartbeat_age_ms));
    put("info_matches_pose", s.info_matches_pose.into());
    put("status_matches_pose", s.status_matches_pose.into());
    put("info", telemetry(&s.info_json)?);
    put("source_status", telemetry(&s.status_json)?);
    let pose = if s.has_pose {
        let floats = |values: &[f32]| Value::Array(values.iter().map(|&v| v.into()).collect());
        let mut pose = Map::new();
        let mut put = |key: &str, value: Value| {
            pose.insert(key.to_owned(), value);
        };
        put("quaternion_xyzw", floats(&s.quaternion_xyzw));
        put("euler_deg", floats(&s.euler_deg));
        put("instance_id", decimal(t.instance_id));
        put("session_id", decimal(t.source_session_id));
        put("sequence", decimal(t.source_sequence));
        put("tx_sequence", decimal(t.tx_sequence));
        put("reference_epoch", decimal(t.reference_epoch));
        put("metadata_revision", decimal(t.metadata_revision));
        put("received_ns", decimal(t.source_received_ns));
        put("age_at_send_ns", decimal(t.source_age_at_send_ns));
        put("sample_time_kind", t.sample_time_kind.into());
        put("sample_time_ms", decimal(t.sample_time_ms));
        put("sample_clock_epoch", decimal(t.sample_clock_epoch));
        Value::Object(pose)
    } else {
        Value::Null
    };
    result.insert("pose".to_owned(), pose);
    Ok(mradm_json::dump(&Value::Object(result), None))
}

#[cfg(test)]
mod tests;
