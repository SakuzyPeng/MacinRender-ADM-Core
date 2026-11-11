use super::*;
use crate::tests::{euler, info_json, pose_packet, status_json, telemetry, timing};
use std::net::UdpSocket;

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
        Self(UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap())
    }
    fn send(&self, receiver: &Receiver, bytes: &[u8]) {
        let before = receiver.snapshot();
        self.0
            .send_to(bytes, (Ipv4Addr::LOCALHOST, before.bound_port))
            .unwrap();
        wait_until(|| receiver.snapshot().packets_received > before.packets_received);
    }
}

fn started(source: &str) -> Receiver {
    let receiver = Receiver::new(0, source.as_bytes(), bind_loopback);
    receiver.start().unwrap();
    receiver
}

fn advance(t: &mut Timing) {
    t.source_sequence += 1;
    t.tx_sequence += 1;
    t.source_received_ns += 10_000_000;
}

#[test]
fn binds_loopback_once_and_releases_on_stop() {
    let receiver = Receiver::new(0, b"", bind_loopback);
    assert_eq!(receiver.snapshot().state, State::Idle);
    receiver.start().unwrap();
    let first = receiver.snapshot();
    assert_eq!(first.state, State::Waiting);
    assert_ne!(first.bound_port, 0);
    receiver.start().unwrap();
    assert_eq!(
        receiver.snapshot().session_id,
        first.session_id,
        "idempotent start"
    );

    let occupied = Receiver::new(first.bound_port, b"", bind_loopback);
    let error = occupied.start().unwrap_err();
    assert_eq!(error.code, IO_ERROR);
    assert!(
        error
            .message
            .starts_with("OSC 回环端口绑定失败，端口可能已被占用 (")
    );
    assert_eq!(occupied.last_error(), error.message);
    let failed = occupied.snapshot();
    assert_eq!((failed.state, failed.bound_port), (State::Failed, 0));

    receiver.stop();
    receiver.stop();
    assert_eq!(receiver.snapshot().state, State::Stopped);
    assert_eq!(receiver.snapshot().bound_port, 0);
    occupied.start().unwrap();
    assert_eq!(
        occupied.snapshot().bound_port,
        first.bound_port,
        "port released"
    );
    assert!(occupied.last_error().is_empty());
    drop(occupied);
    let reuse = Receiver::new(first.bound_port, b"", bind_loopback);
    reuse.start().unwrap();

    receiver.start().unwrap();
    assert!(
        receiver.snapshot().session_id > first.session_id,
        "restart is a new session"
    );
}

#[test]
fn rejects_an_invalid_source_filter_before_binding() {
    let receiver = Receiver::new(0, b"\n", bind_loopback);
    let error = receiver.start().unwrap_err();
    assert_eq!(error.code, INVALID_ARGUMENT);
    assert_eq!(
        error.message,
        "source_id must be 1..256 valid UTF-8 bytes without controls"
    );
    assert_eq!(receiver.snapshot().state, State::Idle);
}

#[test]
fn reports_a_failed_bind_with_its_code() {
    fn refuse(_: u16) -> Result<Bound, BindError> {
        Err(BindError {
            message: "OSC UDP socket 创建失败",
            code: 24,
        })
    }
    let receiver = Receiver::new(0, b"", refuse);
    let error = receiver.start().unwrap_err();
    assert_eq!(error.message, "OSC UDP socket 创建失败 (24)");
    assert_eq!(receiver.last_error(), "OSC UDP socket 创建失败 (24)");
}

#[test]
fn filters_sources_and_orders_poses() {
    let sender = Sender::new();
    let receiver = started("head");
    let mut t = timing();
    sender.send(
        &receiver,
        &pose_packet("/posebridge/euler", &[60., 20., 10.], &t, b"other"),
    );
    assert_eq!(receiver.snapshot().ignored_sources, 1);
    assert!(!receiver.snapshot().has_pose);

    sender.send(&receiver, &euler(&t));
    let s = receiver.snapshot();
    assert!(s.has_pose && s.fresh && s.sequence == 1 && s.state == State::Active);
    assert!((s.euler_deg[0] - 30.).abs() < 5e-4);

    sender.send(&receiver, &euler(&t));
    assert_eq!(receiver.snapshot().rejected_packets, 1, "duplicate");
    assert_eq!(receiver.snapshot().received_ns, s.received_ns);

    advance(&mut t);
    t.tx_sequence += 1;
    sender.send(&receiver, &euler(&t));
    let s = receiver.snapshot();
    assert_eq!((s.sequence, s.missing_tx_packets), (2, 1));

    sender.send(&receiver, b"garbage!");
    assert_eq!(receiver.snapshot().rejected_packets, 2);

    let mut mismatch = Vec::new();
    crate::tests::text(&mut mismatch, b"/posebridge/v1/euler");
    crate::tests::text(&mut mismatch, b",fff");
    mismatch.extend_from_slice(&[0; 12]);
    sender.send(&receiver, &mismatch);
    assert_eq!(receiver.snapshot().protocol_mismatches, 1);
    assert_eq!(
        receiver.last_error(),
        "PoseBridge protocol mismatch: receiver requires protocol 3"
    );
}

#[test]
fn matches_telemetry_to_the_pose_and_goes_stale() {
    let sender = Sender::new();
    let receiver = started("");
    let t = timing();
    sender.send(&receiver, &telemetry(info_json().as_bytes(), true));
    let s = receiver.snapshot();
    assert_eq!(s.telemetry_packets, 1);
    assert!(!s.info_matches_pose, "metadata cannot claim a first pose");
    sender.send(&receiver, &euler(&t));
    sender.send(
        &receiver,
        &telemetry(status_json("active").as_bytes(), false),
    );
    let s = receiver.snapshot();
    assert!(s.info_matches_pose && s.status_matches_pose && s.has_heartbeat);
    assert_eq!(s.info_json, info_json());
    assert!(s.fresh && s.heartbeat_alive);

    std::thread::sleep(Duration::from_millis(520));
    let s = receiver.snapshot();
    assert_eq!(s.state, State::Stale);
    assert!(!s.fresh && s.age_ms >= 500);
    receiver.stop();
    let s = receiver.snapshot();
    assert!(s.has_pose && !s.fresh && !s.heartbeat_alive);
    assert_eq!(s.state, State::Stopped);
}

#[test]
fn snapshot_json_is_the_nlohmann_dump() {
    let empty = Snapshot::default();
    assert_eq!(
        snapshot_json(&empty).unwrap(),
        concat!(
            r#"{"age_ms":"0","bound_port":0,"fresh":false,"has_heartbeat":false,"has_pose":false,"#,
            r#""heartbeat_age_ms":"0","heartbeat_alive":false,"ignored_sources":"0","info":null,"#,
            r#""info_matches_pose":false,"missing_tx_packets":"0","packets_received":"0","pose":null,"#,
            r#""pose_packets":"0","protocol_mismatches":"0","received_ns":"0","receiver_sequence":"0","#,
            r#""receiver_session_id":"0","recovery_count":"0","rejected_packets":"0","schema":3,"#,
            r#""source_id":"","source_status":null,"state":0,"status_matches_pose":false,"#,
            r#""telemetry_packets":"0"}"#
        )
    );
    let mut s = Snapshot {
        state: State::Active,
        bound_port: 9000,
        has_pose: true,
        source_id: "head".into(),
        info_json: "\u{feff}{\"b\":1e2,\"a\":[1.5,-0,18446744073709551616]}".into(),
        quaternion_xyzw: [0.1, 0., 0., 1.],
        euler_deg: [30., -20.5, 0.],
        timing: timing(),
        ..Snapshot::default()
    };
    s.timing.sample_time_kind = 2;
    let json = snapshot_json(&s).unwrap();
    assert!(
        json.contains(r#""info":{"a":[1.5,0,1.8446744073709552e+19],"b":100.0}"#),
        "{json}"
    );
    assert!(json.contains(
        r#""pose":{"age_at_send_ns":"0","euler_deg":[30.0,-20.5,0.0],"instance_id":"123","#
    ));
    assert!(json.contains(r#""quaternion_xyzw":[0.10000000149011612,0.0,0.0,1.0],"#));
    assert!(json.contains(r#""sample_time_kind":2,"#));
    assert!(json.contains(r#""state":2,"#));
    s.status_json = "{".into();
    assert!(snapshot_json(&s).is_err());
}
