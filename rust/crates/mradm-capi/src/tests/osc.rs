//! The PoseBridge receiver through its C ABI over loopback UDP (from `osc_head_tracking_test`):
//! telemetry number bounds, instance ordering, status reference changes, the receiver contract
//! (filter, freshness, heartbeat, recovery, replay, port ownership, restart), automatic binding
//! and the argument checks.
use super::common::*;
use super::common::{taken, text};
use crate::osc::*;
use mradm_json::{Map, Value};
use std::ffi::CStr;
use std::net::UdpSocket;
use std::ptr;
use std::time::{Duration, Instant};

/// The PoseBridge pose timing fields as the sender writes them.
#[derive(Clone)]
struct Timing {
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

fn fixture() -> Timing {
    Timing {
        protocol_version: 3,
        sample_time_kind: 0,
        instance_id: 123,
        tx_sequence: 1,
        reference_epoch: 1,
        metadata_revision: 1,
        source_age_at_send_ns: 0,
        source_session_id: 456,
        source_sequence: 1,
        source_received_ns: 0,
        sample_time_ms: 0,
        sample_clock_epoch: 0,
    }
}

fn advance(t: &mut Timing) {
    t.source_sequence += 1;
    t.tx_sequence += 1;
    t.source_received_ns += 10_000_000;
    if t.sample_time_kind != 0 {
        t.sample_time_ms += 10;
    }
}

fn osc_text(bytes: &mut Vec<u8>, value: &str) {
    bytes.extend_from_slice(value.as_bytes());
    bytes.push(0);
    while !bytes.len().is_multiple_of(4) {
        bytes.push(0);
    }
}

fn packet(address: &str, values: &[f32], t: &Timing, source: &str) -> Vec<u8> {
    let mut bytes = Vec::new();
    osc_text(&mut bytes, address);
    osc_text(
        &mut bytes,
        &format!(",ishhhhhhhhihh{}", "f".repeat(values.len())),
    );
    bytes.extend_from_slice(&t.protocol_version.to_be_bytes());
    osc_text(&mut bytes, source);
    for v in [
        t.instance_id,
        t.source_session_id,
        t.source_sequence,
        t.tx_sequence,
        t.reference_epoch,
        t.metadata_revision,
        t.source_received_ns,
        t.source_age_at_send_ns,
    ] {
        bytes.extend_from_slice(&v.to_be_bytes());
    }
    bytes.extend_from_slice(&t.sample_time_kind.to_be_bytes());
    bytes.extend_from_slice(&t.sample_time_ms.to_be_bytes());
    bytes.extend_from_slice(&t.sample_clock_epoch.to_be_bytes());
    for v in values {
        bytes.extend_from_slice(&v.to_bits().to_be_bytes());
    }
    bytes
}

fn euler(t: &Timing, yaw: f32, source: &str) -> Vec<u8> {
    packet("/posebridge/euler", &[yaw, 20.0, 10.0], t, source)
}

fn object(entries: Vec<(&str, Value)>) -> Value {
    Value::Object(
        entries
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect::<Map>(),
    )
}

fn s(value: impl ToString) -> Value {
    Value::String(value.to_string())
}

/// A telemetry document, serialised as the C++ test's nlohmann object (sorted keys, compact).
fn metadata(t: &Timing, sequence: u64, info: bool, source: &str, state: &str) -> String {
    let mut entries = vec![
        ("schema", Value::Unsigned(3)),
        ("kind", s(if info { "info" } else { "status" })),
        ("source_id", s(source)),
        ("instance_id", s(t.instance_id)),
        ("session_id", s(t.source_session_id)),
        ("reference_epoch", s(t.reference_epoch)),
        ("metadata_revision", s(t.metadata_revision)),
        ("message_seq", s(sequence)),
    ];
    if info {
        entries.push((
            "descriptor",
            object(vec![
                ("source_id", s(source)),
                ("instance_id", s(t.instance_id)),
                ("session_id", s(t.source_session_id)),
                ("reference_epoch", s(t.reference_epoch)),
                ("metadata_revision", s(t.metadata_revision)),
                ("coordinate_profile", s("posebridge.yxz.v1")),
            ]),
        ));
    } else {
        entries.push((
            "status",
            object(vec![
                ("session_id", s(t.source_session_id)),
                ("session_samples", s(t.source_sequence)),
                ("state", s(state)),
            ]),
        ));
    }
    mradm_json::dump(&object(entries), None)
}

fn telemetry_text(json: &str, info: bool) -> Vec<u8> {
    let mut bytes = Vec::new();
    osc_text(
        &mut bytes,
        if info {
            "/posebridge/info"
        } else {
            "/posebridge/status"
        },
    );
    osc_text(&mut bytes, ",s");
    osc_text(&mut bytes, json);
    bytes
}

fn telemetry(t: &Timing, sequence: u64, info: bool) -> Vec<u8> {
    telemetry_text(&metadata(t, sequence, info, "head", "active"), info)
}

fn status_text(t: &Timing, sequence: u64, state: &str) -> Vec<u8> {
    telemetry_text(&metadata(t, sequence, false, "head", state), false)
}

/// A receiver, destroyed on drop.
struct Receiver(*mut adm_osc_head_tracking_t);

impl Receiver {
    fn new(port: u32, source: Option<&CStr>) -> Self {
        let config = adm_osc_head_tracking_config_t {
            struct_size: size_of::<adm_osc_head_tracking_config_t>() as u32,
            listen_port: port,
            source_id: source.map_or(ptr::null(), CStr::as_ptr),
        };
        let mut handle = ptr::null_mut();
        assert_eq!(
            unsafe { adm_create_osc_head_tracking(&config, &mut handle) },
            OK
        );
        assert!(!handle.is_null());
        Self(handle)
    }

    fn start(&self) -> i32 {
        unsafe { adm_osc_head_tracking_start(self.0) }
    }

    fn pose(&self) -> adm_head_tracking_pose_t {
        // SAFETY: plain data whose all-zero value is valid.
        let mut p: adm_head_tracking_pose_t = unsafe { std::mem::zeroed() };
        p.struct_size = size_of::<adm_head_tracking_pose_t>() as u32;
        assert_eq!(
            unsafe { adm_osc_head_tracking_get_pose(self.0, &mut p) },
            OK
        );
        p
    }

    fn status(&self) -> adm_osc_head_tracking_status_t {
        // SAFETY: plain data whose all-zero value is valid.
        let mut st: adm_osc_head_tracking_status_t = unsafe { std::mem::zeroed() };
        st.struct_size = size_of::<adm_osc_head_tracking_status_t>() as u32;
        assert_eq!(
            unsafe { adm_osc_head_tracking_get_status(self.0, &mut st) },
            OK
        );
        st
    }

    fn snapshot(&self) -> Value {
        let mut out = ptr::null_mut();
        assert_eq!(
            unsafe { adm_osc_head_tracking_snapshot_json(self.0, &mut out) },
            OK
        );
        let text = unsafe { taken(out) }.unwrap();
        mradm_json::parse(text.as_bytes()).unwrap()
    }
}

impl Drop for Receiver {
    fn drop(&mut self) {
        unsafe { adm_destroy_osc_head_tracking(self.0) };
    }
}

fn wait_until(mut done: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(4);
    while !done() {
        assert!(Instant::now() < deadline, "receiver timeout");
        std::thread::sleep(Duration::from_millis(2));
    }
}

struct Sender(UdpSocket);

impl Sender {
    fn new() -> Self {
        Self(UdpSocket::bind("127.0.0.1:0").unwrap())
    }

    fn send(&self, port: u32, bytes: &[u8]) {
        assert_eq!(
            self.0.send_to(bytes, ("127.0.0.1", port as u16)).unwrap(),
            bytes.len()
        );
    }

    /// Sends and waits for the receiver to count the datagram.
    fn deliver(&self, receiver: &Receiver, bytes: &[u8]) {
        let before = receiver.status();
        self.send(before.bound_port, bytes);
        wait_until(|| receiver.status().packets_received > before.packets_received);
    }
}

fn at<'a>(value: &'a Value, path: &[&str]) -> &'a Value {
    path.iter()
        .fold(value, |v, key| mradm_json::at(v, key).unwrap())
}

fn text_at<'a>(value: &'a Value, path: &[&str]) -> &'a str {
    at(value, path).as_str().unwrap()
}

fn flag_at(value: &Value, key: &str) -> bool {
    at(value, &[key]).as_bool().unwrap()
}

#[test]
fn telemetry_numbers_outside_f64_are_rejected_and_its_maximum_survives() {
    let sender = Sender::new();
    let receiver = Receiver::new(0, Some(c"head"));
    assert_eq!(receiver.start(), OK);
    let t = fixture();
    for info in [false, true] {
        let key = if info { "info" } else { "source_status" };
        sender.deliver(&receiver, &telemetry(&t, 1, info));
        for overflow in ["1.79769313486231581e308", "-1.79769313486231581e308"] {
            let mut json = metadata(&t, 2, info, "head", "active");
            json.insert_str(1, &format!("\"extra\":{overflow},"));
            let before = receiver.status();
            sender.deliver(&receiver, &telemetry_text(&json, info));
            // The snapshot is read first so a cached overflow would also show its failure.
            let all = receiver.snapshot();
            let after = receiver.status();
            assert_eq!(after.rejected_packets, before.rejected_packets + 1);
            assert_eq!(after.telemetry_packets, before.telemetry_packets);
            assert_eq!(text_at(&all, &[key, "message_seq"]), "1");
        }
        let mut json = metadata(&t, 2, info, "head", "active");
        json.insert_str(1, "\"extra\":1.7976931348623157e308,");
        sender.deliver(&receiver, &telemetry_text(&json, info));
        let all = receiver.snapshot();
        assert_eq!(text_at(&all, &[key, "message_seq"]), "2");
        assert!(matches!(at(&all, &[key, "extra"]), Value::Float(v) if *v == f64::MAX));
    }
}

#[test]
fn telemetry_follows_the_pose_instance() {
    let sender = Sender::new();
    let receiver = Receiver::new(0, Some(c"head"));
    assert_eq!(receiver.start(), OK);
    let mut t = fixture();
    sender.deliver(&receiver, &euler(&t, 30.0, "head"));
    let mut stopped = t.clone();
    stopped.source_sequence += 2;
    sender.deliver(&receiver, &telemetry(&t, 10, true));
    sender.deliver(&receiver, &status_text(&stopped, 10, "stopped"));
    assert_eq!(
        receiver.pose().fresh,
        0,
        "a confirmed stop invalidates the pose"
    );

    // More candidates than the bounded cache holds cannot evict the current instance.
    let mut candidate = fixture();
    for i in 1..=20 {
        candidate.instance_id = t.instance_id + i;
        candidate.source_session_id = 0;
        candidate.source_sequence = 0;
        sender.deliver(&receiver, &telemetry(&candidate, 1, true));
        sender.deliver(&receiver, &status_text(&candidate, 1, "connecting"));
    }
    let all = receiver.snapshot();
    assert_eq!(text_at(&all, &["info", "message_seq"]), "10");
    assert_eq!(text_at(&all, &["source_status", "message_seq"]), "10");
    let before = receiver.status();
    sender.deliver(&receiver, &telemetry(&stopped, 9, false));
    sender.deliver(&receiver, &telemetry(&t, 9, true));
    assert_eq!(
        receiver.status().rejected_packets,
        before.rejected_packets + 2
    );
    assert_eq!(receiver.pose().fresh, 0);
    advance(&mut t);
    sender.deliver(&receiver, &euler(&t, 30.0, "head"));
    assert_eq!(
        receiver.pose().fresh,
        0,
        "the cached stop covers a delayed earlier pose"
    );
    advance(&mut t);
    t.metadata_revision += 1;
    sender.deliver(&receiver, &euler(&t, 30.0, "head"));
    let all = receiver.snapshot();
    assert!(!flag_at(&all, "info_matches_pose") && !flag_at(&all, "status_matches_pose"));
    assert_ne!(receiver.pose().fresh, 0);

    candidate.source_session_id = fixture().source_session_id;
    candidate.source_sequence = 1;
    candidate.metadata_revision = 2;
    sender.deliver(&receiver, &telemetry(&candidate, 2, true));
    sender.deliver(&receiver, &telemetry(&candidate, 2, false));
    assert_eq!(
        receiver.pose().instance_id,
        t.instance_id,
        "telemetry cannot take over the binding"
    );
    sender.deliver(&receiver, &euler(&candidate, 30.0, "head"));
    let all = receiver.snapshot();
    assert_eq!(receiver.pose().instance_id, candidate.instance_id);
    assert!(flag_at(&all, "info_matches_pose") && flag_at(&all, "status_matches_pose"));
    assert_eq!(text_at(&all, &["source_status", "message_seq"]), "2");
    let rejected = receiver.status().rejected_packets;
    sender.deliver(&receiver, &telemetry(&t, 11, false));
    assert_eq!(
        receiver.status().rejected_packets,
        rejected + 1,
        "a retired instance stays rejected"
    );
}

#[test]
fn a_status_reference_change_invalidates_the_pose() {
    let sender = Sender::new();
    for with_info in [false, true] {
        let receiver = Receiver::new(0, Some(c"head"));
        assert_eq!(receiver.start(), OK);
        let mut t = fixture();
        sender.deliver(&receiver, &euler(&t, 30.0, "head"));
        if with_info {
            sender.deliver(&receiver, &telemetry(&t, 1, true));
        }
        let mut newer = t.clone();
        newer.metadata_revision += 1;
        sender.deliver(&receiver, &telemetry(&newer, 1, false));
        assert_ne!(
            receiver.pose().fresh,
            0,
            "a metadata-only change keeps the reference"
        );
        newer.metadata_revision += 1;
        newer.reference_epoch += 1;
        sender.deliver(&receiver, &telemetry(&newer, 2, false));
        assert_eq!(receiver.pose().fresh, 0);
        assert_ne!(receiver.status().heartbeat_alive, 0);
        advance(&mut t);
        sender.deliver(&receiver, &euler(&t, 30.0, "head"));
        assert_eq!(
            receiver.pose().fresh,
            0,
            "a later sample of the old reference does not undo it"
        );
        advance(&mut t);
        t.metadata_revision = newer.metadata_revision;
        t.reference_epoch = newer.reference_epoch;
        sender.deliver(&receiver, &euler(&t, 30.0, "head"));
        assert_ne!(receiver.pose().fresh, 0);
        assert!(flag_at(&receiver.snapshot(), "status_matches_pose"));
        let rejected = receiver.status().rejected_packets;
        newer.reference_epoch -= 1;
        sender.deliver(&receiver, &telemetry(&newer, 3, false));
        assert_eq!(receiver.status().rejected_packets, rejected + 1);
        assert_ne!(receiver.pose().fresh, 0);
    }
}

/// One receiver across its lifecycle: no first pose until one arrives, an idempotent start, an
/// exclusive port, the source filter, matching metadata, a heartbeat that never refreshes the
/// orientation, recovery, stopped statuses, coalescing that is not loss, duplicates, instance
/// changes, protocol mismatches, the heartbeat timeout, output records, and port release.
#[test]
fn the_receiver_keeps_its_contract_across_its_lifecycle() {
    let sender = Sender::new();
    let receiver = Receiver::new(0, Some(c"head"));
    assert_eq!(receiver.pose().has_pose, 0);
    assert!(matches!(at(&receiver.snapshot(), &["pose"]), Value::Null));
    assert_eq!(receiver.start(), OK);
    let initial = receiver.status();
    let port = initial.bound_port;
    assert_eq!(receiver.start(), OK);
    assert_eq!(
        receiver.status().session_id,
        initial.session_id,
        "an idempotent start"
    );
    let occupied = Receiver::new(port, None);
    assert_eq!(occupied.start(), IO, "the port is exclusive");
    let mut t = fixture();
    sender.send(port, &euler(&t, 60.0, "other"));
    wait_until(|| receiver.status().ignored_sources == 1);
    assert_eq!(receiver.pose().has_pose, 0, "the source filter");
    sender.send(port, &euler(&t, 30.0, "head"));
    wait_until(|| receiver.pose().receiver_sequence == 1);
    let first = receiver.pose();
    assert!(first.fresh != 0 && (first.yaw_deg - 30.0).abs() < 0.0005);
    sender.send(port, &telemetry(&t, 1, true));
    sender.send(port, &telemetry(&t, 1, false));
    wait_until(|| receiver.status().telemetry_packets == 2);
    let all = receiver.snapshot();
    assert!(flag_at(&all, "info_matches_pose") && flag_at(&all, "status_matches_pose"));
    assert_eq!(text_at(&all, &["pose", "instance_id"]), "123");
    let mut sequence = 2;
    let end = Instant::now() + Duration::from_millis(620);
    while Instant::now() < end {
        sender.send(port, &telemetry(&t, sequence, false));
        sequence += 1;
        std::thread::sleep(Duration::from_millis(40));
    }
    let p = receiver.pose();
    assert!(p.fresh == 0 && receiver.status().heartbeat_alive != 0 && p.receiver_sequence == 1);
    advance(&mut t);
    sender.send(port, &euler(&t, 30.0, "head"));
    wait_until(|| receiver.pose().receiver_sequence == 2);
    assert!(receiver.pose().fresh != 0 && receiver.status().recovery_count == 1);
    let mut old = t.clone();
    old.source_sequence -= 1;
    sender.send(port, &status_text(&old, sequence, "stopped"));
    sequence += 1;
    std::thread::sleep(Duration::from_millis(30));
    assert_ne!(
        receiver.pose().fresh,
        0,
        "an older stop cannot override a newer sample"
    );
    sender.send(port, &status_text(&t, sequence, "stopped"));
    sequence += 1;
    wait_until(|| receiver.pose().fresh == 0);
    advance(&mut t);
    t.source_sequence += 6;
    t.tx_sequence += 1;
    sender.send(port, &euler(&t, 30.0, "head"));
    wait_until(|| receiver.pose().receiver_sequence == 3);
    assert_eq!(
        receiver.status().missing_tx_packets,
        1,
        "sample coalescing is not UDP loss"
    );
    let accepted = receiver.pose();
    let rejected = receiver.status().rejected_packets;
    sender.send(port, &euler(&t, 30.0, "head"));
    wait_until(|| receiver.status().rejected_packets > rejected);
    assert_eq!(
        receiver.pose().receiver_received_ns,
        accepted.receiver_received_ns
    );
    let mut next = fixture();
    next.instance_id += 1;
    sender.send(port, &euler(&next, 45.0, "head"));
    wait_until(|| receiver.pose().instance_id == next.instance_id);
    let total = receiver.status().packets_received;
    sender.send(port, &telemetry(&t, sequence, false));
    sender.send(port, &euler(&t, 30.0, "head"));
    wait_until(|| receiver.status().packets_received >= total + 2);
    assert_eq!(
        receiver.pose().instance_id,
        next.instance_id,
        "a retired instance stays out"
    );
    sender.send(
        port,
        &packet("/posebridge/v1/euler", &[0.0; 3], &fixture(), "head"),
    );
    wait_until(|| receiver.status().protocol_mismatches == 1);
    assert!(
        !unsafe { text(adm_osc_head_tracking_last_error_message(receiver.0)) }
            .unwrap()
            .is_empty()
    );
    sender.send(port, &telemetry(&next, 1, false));
    wait_until(|| receiver.status().has_heartbeat != 0);
    std::thread::sleep(Duration::from_millis(3050));
    assert!(
        receiver.status().heartbeat_alive == 0 && receiver.pose().fresh == 0,
        "the heartbeat times out"
    );

    #[repr(C)]
    struct Future {
        pose: adm_head_tracking_pose_t,
        tail: u64,
    }
    // SAFETY: plain data whose all-zero value is valid.
    let mut future: Future = unsafe { std::mem::zeroed() };
    future.pose.struct_size = size_of::<Future>() as u32;
    future.tail = 42;
    assert_eq!(
        unsafe { adm_osc_head_tracking_get_pose(receiver.0, &mut future.pose) },
        OK
    );
    assert_eq!(future.tail, 42, "an unknown tail is left alone");
    future.pose.struct_size = 4;
    assert_eq!(
        unsafe { adm_osc_head_tracking_get_pose(receiver.0, &mut future.pose) },
        INVALID_ARGUMENT
    );
    assert_eq!(future.pose.struct_size, 4);
    unsafe {
        adm_osc_head_tracking_stop(receiver.0);
        adm_osc_head_tracking_stop(receiver.0);
    }
    assert!(receiver.pose().fresh == 0 && receiver.status().heartbeat_alive == 0);
    assert_eq!(occupied.start(), OK, "stop releases the port");
    drop(occupied);
    assert_eq!(receiver.start(), OK);
    assert_eq!(receiver.pose().has_pose, 0);
    assert_ne!(
        receiver.status().session_id,
        initial.session_id,
        "a restart clears the binding"
    );
    let final_port = receiver.status().bound_port;
    drop(receiver);
    let reuse = Receiver::new(final_port, None);
    assert_eq!(
        reuse.start(),
        OK,
        "destroying the receiver releases its port"
    );
}

#[test]
fn the_first_pose_source_is_bound_automatically() {
    let sender = Sender::new();
    let receiver = Receiver::new(0, None);
    assert_eq!(receiver.start(), OK);
    let port = receiver.status().bound_port;
    let mut t = fixture();
    sender.send(
        port,
        &telemetry_text(&metadata(&t, 1, true, "metadata-only", "active"), true),
    );
    sender.send(port, &euler(&t, 30.0, "chosen"));
    wait_until(|| receiver.pose().has_pose != 0);
    let all = receiver.snapshot();
    assert_eq!(text_at(&all, &["source_id"]), "chosen");
    assert!(
        !flag_at(&all, "info_matches_pose"),
        "metadata cannot claim the first-pose binding"
    );
    advance(&mut t);
    sender.send(port, &euler(&t, 60.0, "other"));
    wait_until(|| receiver.status().ignored_sources == 1);
    assert!((receiver.pose().yaw_deg - 30.0).abs() < 0.0005);
}

#[test]
fn receivers_check_their_arguments() {
    let mut handle = ptr::null_mut();
    let mut config = adm_osc_head_tracking_config_t {
        struct_size: size_of::<adm_osc_head_tracking_config_t>() as u32,
        listen_port: 65536,
        source_id: ptr::null(),
    };
    unsafe {
        assert_eq!(
            adm_create_osc_head_tracking(&config, &mut handle),
            INVALID_ARGUMENT
        );
        config.listen_port = 0;
        config.source_id = c"\n".as_ptr();
        assert_eq!(
            adm_create_osc_head_tracking(&config, &mut handle),
            INVALID_ARGUMENT
        );
        assert_eq!(
            adm_create_osc_head_tracking(ptr::null(), ptr::null_mut()),
            INVALID_ARGUMENT
        );
        let mut untouched = 0 as std::ffi::c_char;
        let mut out = &raw mut untouched;
        assert_eq!(
            adm_osc_head_tracking_snapshot_json(ptr::null(), &mut out),
            INVALID_ARGUMENT
        );
        assert!(out.is_null());
        assert_eq!(
            adm_osc_head_tracking_get_pose(ptr::null(), ptr::null_mut()),
            INVALID_ARGUMENT
        );
        adm_osc_head_tracking_stop(ptr::null_mut());
        adm_destroy_osc_head_tracking(ptr::null_mut());
    }
}
