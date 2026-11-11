use crate::{boundary, slice};
use mradm_dsp::Error;
use mradm_osc as osc;
use mradm_osc::receiver;
use std::panic::{AssertUnwindSafe, catch_unwind};

/// Same field order as `mradm::HeadTrackingTiming`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OscTiming {
    protocol_version: u32,
    sample_time_kind: u32,
    instance_id: u64,
    tx_sequence: u64,
    reference_epoch: u64,
    metadata_revision: u64,
    source_age_at_send_ns: u64,
    source_session_id: u64,
    source_sequence: u64,
    source_received_ns: u64,
    sample_time_ms: u64,
    sample_clock_epoch: u64,
}

impl From<osc::Timing> for OscTiming {
    fn from(t: osc::Timing) -> Self {
        Self {
            protocol_version: t.protocol_version,
            sample_time_kind: t.sample_time_kind,
            instance_id: t.instance_id,
            tx_sequence: t.tx_sequence,
            reference_epoch: t.reference_epoch,
            metadata_revision: t.metadata_revision,
            source_age_at_send_ns: t.source_age_at_send_ns,
            source_session_id: t.source_session_id,
            source_sequence: t.source_sequence,
            source_received_ns: t.source_received_ns,
            sample_time_ms: t.sample_time_ms,
            sample_clock_epoch: t.sample_clock_epoch,
        }
    }
}

impl From<OscTiming> for osc::Timing {
    fn from(t: OscTiming) -> Self {
        Self {
            protocol_version: t.protocol_version,
            sample_time_kind: t.sample_time_kind,
            instance_id: t.instance_id,
            tx_sequence: t.tx_sequence,
            reference_epoch: t.reference_epoch,
            metadata_revision: t.metadata_revision,
            source_age_at_send_ns: t.source_age_at_send_ns,
            source_session_id: t.source_session_id,
            source_sequence: t.source_sequence,
            source_received_ns: t.source_received_ns,
            sample_time_ms: t.sample_time_ms,
            sample_clock_epoch: t.sample_clock_epoch,
        }
    }
}

/// kind: 0 pose, 1 info, 2 status, 3 incompatible. Telemetry source ids come from JSON
/// escapes, so they are copied; the JSON text stays in the caller's datagram.
#[repr(C)]
pub struct OscMessage {
    kind: u32,
    source_active: u32,
    quaternion_xyzw: [f32; 4],
    euler_deg: [f32; 3],
    timing: OscTiming,
    message_sequence: u64,
    reported_samples: u64,
    source_id_len: usize,
    source_id: [u8; osc::MAX_SOURCE_ID],
    json_offset: usize,
    json_len: usize,
}

/// Plain state owned by C++; all-zero is the initial state.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OscSourceOrder {
    last: OscTiming,
    clock: OscTiming,
    retired: [u64; 16],
    retired_next: usize,
    last_gap: u64,
}

impl From<OscSourceOrder> for osc::SourceOrder {
    fn from(o: OscSourceOrder) -> Self {
        Self {
            last: o.last.into(),
            clock: o.clock.into(),
            retired: o.retired,
            retired_next: o.retired_next,
            last_gap: o.last_gap,
        }
    }
}

impl From<osc::SourceOrder> for OscSourceOrder {
    fn from(o: osc::SourceOrder) -> Self {
        Self {
            last: o.last.into(),
            clock: o.clock.into(),
            retired: o.retired,
            retired_next: o.retired_next,
            last_gap: o.last_gap,
        }
    }
}

/// Returns 0 with `out` filled for a decoded datagram, nonzero for any rejected input.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_decode(
    data: *const u8,
    len: usize,
    out: *mut OscMessage,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || {
        if out.is_null() || (data.is_null() && len != 0) {
            return Err(Error::InvalidArgument("Null OSC argument"));
        }
        let bytes = if len == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(data, len) }
        };
        let msg = osc::decode(bytes).ok_or(Error::InvalidArgument("Rejected OSC datagram"))?;
        let mut source_id = [0u8; osc::MAX_SOURCE_ID];
        source_id[..msg.source_id.len()].copy_from_slice(msg.source_id.as_bytes());
        let result = OscMessage {
            kind: match msg.kind {
                osc::Kind::Pose => 0,
                osc::Kind::Info => 1,
                osc::Kind::Status => 2,
                osc::Kind::Incompatible => 3,
            },
            source_active: u32::from(msg.source_active),
            quaternion_xyzw: msg.quaternion_xyzw,
            euler_deg: msg.euler_deg,
            timing: msg.timing.into(),
            message_sequence: msg.message_sequence,
            reported_samples: msg.reported_samples,
            source_id_len: msg.source_id.len(),
            source_id,
            json_offset: msg.json.start,
            json_len: msg.json.len(),
        };
        unsafe { out.write(result) };
        Ok(())
    })
}

fn guarded(f: impl FnOnce() -> bool) -> u32 {
    catch_unwind(AssertUnwindSafe(f)).map_or(0, u32::from)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_valid_source_id(data: *const u8, len: usize) -> u32 {
    guarded(|| !data.is_null() && osc::valid_source_id(unsafe { slice::from_raw_parts(data, len) }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_source_order_accept(
    order: *mut OscSourceOrder,
    timing: *const OscTiming,
) -> u32 {
    guarded(|| {
        if order.is_null() || timing.is_null() {
            return false;
        }
        let mut state = osc::SourceOrder::from(unsafe { *order });
        let accepted = state.accept(&unsafe { *timing }.into());
        unsafe { *order = state.into() };
        accepted
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_source_order_retired(
    order: *const OscSourceOrder,
    instance_id: u64,
) -> u32 {
    guarded(|| !order.is_null() && osc::SourceOrder::from(unsafe { *order }).retired(instance_id))
}

// ── Loopback receiver (formerly C++ `OscHeadTrackingReceiver`) ──────────────────────────────

/// An owned UTF-8 text handed to C++; read with `mradm_osc_text_view`.
pub struct OscText(String);

fn text_out(out: *mut *mut OscText, text: String) {
    if !out.is_null() {
        unsafe { *out = Box::into_raw(Box::new(OscText(text))) };
    }
}

/// Returns the text's length and stores its bytes (valid until `mradm_osc_text_destroy`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_text_view(text: *const OscText, data: *mut *const u8) -> usize {
    if text.is_null() || data.is_null() {
        return 0;
    }
    let text = unsafe { &(*text).0 };
    unsafe { *data = text.as_ptr() };
    text.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_text_destroy(text: *mut OscText) {
    if !text.is_null() {
        drop(unsafe { Box::from_raw(text) });
    }
}

/// `OscHeadTrackingSnapshot` without the telemetry text; flags are 0 or 1.
#[repr(C)]
pub struct OscSnapshot {
    state: i32,
    bound_port: u32,
    has_pose: u32,
    fresh: u32,
    has_heartbeat: u32,
    heartbeat_alive: u32,
    info_matches_pose: u32,
    status_matches_pose: u32,
    session_id: u64,
    sequence: u64,
    received_ns: u64,
    age_ms: u64,
    packets_received: u64,
    rejected_packets: u64,
    recovery_count: u64,
    pose_packets: u64,
    telemetry_packets: u64,
    ignored_sources: u64,
    protocol_mismatches: u64,
    missing_tx_packets: u64,
    heartbeat_age_ms: u64,
    quaternion_xyzw: [f32; 4],
    euler_deg: [f32; 3],
    timing: OscTiming,
    source_id_len: usize,
    source_id: [u8; osc::MAX_SOURCE_ID],
}

pub struct OscReceiver(receiver::Receiver);

#[cfg(not(windows))]
const BIND: receiver::Bind = receiver::bind_loopback;
#[cfg(windows)]
const BIND: receiver::Bind = winsock::bind;

/// Null for an invalid argument or a panic. `source` (may be empty) is checked by `start`,
/// as the C++ receiver did.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_create(
    port: u16,
    source: *const u8,
    source_len: usize,
) -> *mut OscReceiver {
    if source.is_null() && source_len != 0 {
        return std::ptr::null_mut();
    }
    catch_unwind(AssertUnwindSafe(|| {
        let bytes = if source_len == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(source, source_len) }
        };
        Box::into_raw(Box::new(OscReceiver(receiver::Receiver::new(
            port, bytes, BIND,
        ))))
    }))
    .unwrap_or(std::ptr::null_mut())
}

/// Stops the worker and releases the port.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_destroy(receiver: *mut OscReceiver) {
    if !receiver.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(unsafe { Box::from_raw(receiver) })
        }));
    }
}

/// 0, or the `adm::ErrorCode` with its message in `message` (owned by the caller).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_start(
    receiver: *const OscReceiver,
    message: *mut *mut OscText,
) -> i32 {
    if receiver.is_null() {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| unsafe { &(*receiver).0 }.start()));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            text_out(message, e.message);
            e.code
        }
        Err(_) => {
            text_out(message, "OSC 接收器启动异常".to_owned());
            6
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_stop(receiver: *const OscReceiver) {
    if !receiver.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { &(*receiver).0 }.stop()));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_snapshot(
    receiver: *const OscReceiver,
    out: *mut OscSnapshot,
) -> i32 {
    if receiver.is_null() || out.is_null() {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { &(*receiver).0 }.snapshot();
        let mut source_id = [0u8; osc::MAX_SOURCE_ID];
        let source_id_len = s.source_id.len().min(osc::MAX_SOURCE_ID);
        source_id[..source_id_len].copy_from_slice(&s.source_id.as_bytes()[..source_id_len]);
        OscSnapshot {
            state: s.state as i32,
            bound_port: u32::from(s.bound_port),
            has_pose: u32::from(s.has_pose),
            fresh: u32::from(s.fresh),
            has_heartbeat: u32::from(s.has_heartbeat),
            heartbeat_alive: u32::from(s.heartbeat_alive),
            info_matches_pose: u32::from(s.info_matches_pose),
            status_matches_pose: u32::from(s.status_matches_pose),
            session_id: s.session_id,
            sequence: s.sequence,
            received_ns: s.received_ns,
            age_ms: s.age_ms,
            packets_received: s.packets_received,
            rejected_packets: s.rejected_packets,
            recovery_count: s.recovery_count,
            pose_packets: s.pose_packets,
            telemetry_packets: s.telemetry_packets,
            ignored_sources: s.ignored_sources,
            protocol_mismatches: s.protocol_mismatches,
            missing_tx_packets: s.missing_tx_packets,
            heartbeat_age_ms: s.heartbeat_age_ms,
            quaternion_xyzw: s.quaternion_xyzw,
            euler_deg: s.euler_deg,
            timing: s.timing.into(),
            source_id_len,
            source_id,
        }
    }));
    match result {
        Ok(snapshot) => {
            unsafe { out.write(snapshot) };
            0
        }
        Err(_) => 6,
    }
}

/// 0 with the snapshot JSON in `out`, or 6 with the failure text in `out`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_snapshot_json(
    receiver: *const OscReceiver,
    out: *mut *mut OscText,
) -> i32 {
    if receiver.is_null() || out.is_null() {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        unsafe { &(*receiver).0 }.snapshot_json()
    }));
    let (code, text) = match result {
        Ok(Ok(json)) => (0, json),
        Ok(Err(message)) => (6, message),
        Err(_) => (6, "OSC 快照序列化异常".to_owned()),
    };
    text_out(out, text);
    code
}

/// The last receiver error (empty when none); 6 for a panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_osc_receiver_last_error(
    receiver: *const OscReceiver,
    out: *mut *mut OscText,
) -> i32 {
    if receiver.is_null() || out.is_null() {
        return 1;
    }
    match catch_unwind(AssertUnwindSafe(|| unsafe { &(*receiver).0 }.last_error())) {
        Ok(text) => {
            text_out(out, text);
            0
        }
        Err(_) => 6,
    }
}

/// The Windows bind keeps the former receiver's `SO_EXCLUSIVEADDRUSE`, which `std::net` cannot
/// set before binding: no other socket can share the port, even one asking for address reuse.
#[cfg(windows)]
mod winsock {
    use super::receiver::{BindError, Bound};
    use std::net::UdpSocket;
    use std::os::windows::io::{FromRawSocket, RawSocket};

    type Socket = usize;
    const INVALID_SOCKET: Socket = !0;
    const AF_INET: i32 = 2;
    const SOCK_DGRAM: i32 = 2;
    const IPPROTO_UDP: i32 = 17;
    const SOL_SOCKET: i32 = 0xffff;
    const SO_EXCLUSIVEADDRUSE: i32 = !4;
    // Receive timeouts need an overlapped socket (as `std::net` creates them).
    const WSA_FLAG_OVERLAPPED: u32 = 0x01;
    const WSA_FLAG_NO_HANDLE_INHERIT: u32 = 0x80;

    #[repr(C)]
    struct SockaddrIn {
        family: u16,
        port: u16,
        address: u32,
        zero: [u8; 8],
    }

    #[link(name = "ws2_32")]
    unsafe extern "system" {
        fn WSAStartup(version: u16, data: *mut u64) -> i32;
        fn WSACleanup() -> i32;
        fn WSAGetLastError() -> i32;
        fn WSASocketW(
            family: i32,
            kind: i32,
            protocol: i32,
            info: *const u8,
            group: u32,
            flags: u32,
        ) -> Socket;
        fn setsockopt(socket: Socket, level: i32, name: i32, value: *const u8, len: i32) -> i32;
        #[link_name = "bind"]
        fn bind_socket(socket: Socket, address: *const SockaddrIn, len: i32) -> i32;
        fn closesocket(socket: Socket) -> i32;
    }

    /// Balances this receiver's `WSAStartup`; dropped after its socket.
    struct WinSock;

    impl Drop for WinSock {
        fn drop(&mut self) {
            unsafe { WSACleanup() };
        }
    }

    pub(super) fn bind(port: u16) -> Result<Bound, BindError> {
        // WSADATA is 408 bytes on 64-bit Windows.
        let mut data = [0u64; 64];
        let startup = unsafe { WSAStartup(0x0202, data.as_mut_ptr()) };
        if startup != 0 {
            return Err(BindError {
                message: "OSC WinSock 初始化失败",
                code: startup,
            });
        }
        let winsock = WinSock;
        let fail = |message: &'static str| BindError {
            message,
            code: unsafe { WSAGetLastError() },
        };
        let socket = unsafe {
            WSASocketW(
                AF_INET,
                SOCK_DGRAM,
                IPPROTO_UDP,
                std::ptr::null(),
                0,
                WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT,
            )
        };
        if socket == INVALID_SOCKET {
            return Err(fail("OSC UDP socket 创建失败"));
        }
        let close = |error: BindError| {
            unsafe { closesocket(socket) };
            Err(error)
        };
        let exclusive: i32 = 1;
        let configured = unsafe {
            setsockopt(
                socket,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                (&raw const exclusive).cast(),
                4,
            )
        };
        if configured != 0 {
            return close(fail("OSC UDP socket 配置失败"));
        }
        let address = SockaddrIn {
            family: AF_INET as u16,
            port: port.to_be(),
            address: u32::from(std::net::Ipv4Addr::LOCALHOST).to_be(),
            zero: [0; 8],
        };
        let size = std::mem::size_of::<SockaddrIn>() as i32;
        if unsafe { bind_socket(socket, &address, size) } != 0 {
            return close(fail("OSC 回环端口绑定失败，端口可能已被占用"));
        }
        // SAFETY: a valid, bound socket that the returned `UdpSocket` now owns and closes.
        let socket = unsafe { UdpSocket::from_raw_socket(socket as RawSocket) };
        Ok(Bound {
            socket,
            guard: Some(Box::new(winsock)),
        })
    }
}
