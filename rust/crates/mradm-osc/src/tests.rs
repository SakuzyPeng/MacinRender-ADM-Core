use super::*;

fn text(bytes: &mut Vec<u8>, value: &[u8]) {
    bytes.extend_from_slice(value);
    bytes.push(0);
    while !bytes.len().is_multiple_of(4) {
        bytes.push(0);
    }
}

fn timing() -> Timing {
    Timing {
        protocol_version: PROTOCOL,
        instance_id: 123,
        source_session_id: 456,
        source_sequence: 1,
        tx_sequence: 1,
        reference_epoch: 1,
        metadata_revision: 1,
        ..Timing::default()
    }
}

fn pose_packet(address: &str, values: &[f32], t: &Timing, source: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    text(&mut bytes, address.as_bytes());
    text(
        &mut bytes,
        format!(",ishhhhhhhhihh{}", "f".repeat(values.len())).as_bytes(),
    );
    bytes.extend_from_slice(&t.protocol_version.to_be_bytes());
    text(&mut bytes, source);
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

fn euler(t: &Timing) -> Vec<u8> {
    pose_packet("/posebridge/euler", &[30., 20., 10.], t, b"head")
}

fn status_json(state: &str) -> String {
    format!(
        r#"{{"schema":3,"kind":"status","source_id":"head","instance_id":"123","session_id":"456",
"reference_epoch":"1","metadata_revision":"1","message_seq":"7",
"status":{{"session_id":"456","session_samples":"9","state":"{state}"}}}}"#
    )
}

fn info_json() -> String {
    r#"{"schema":3,"kind":"info","source_id":"head","instance_id":"123","session_id":"456",
"reference_epoch":"1","metadata_revision":"1","message_seq":"7",
"descriptor":{"source_id":"head","instance_id":"123","session_id":"456","reference_epoch":"1",
"metadata_revision":"1","coordinate_profile":"posebridge.yxz.v1"}}"#
        .to_owned()
}

fn telemetry(json: &[u8], info: bool) -> Vec<u8> {
    let mut bytes = Vec::new();
    text(
        &mut bytes,
        if info {
            b"/posebridge/info"
        } else {
            b"/posebridge/status"
        },
    );
    text(&mut bytes, b",s");
    text(&mut bytes, json);
    bytes
}

#[test]
fn decodes_euler_and_quaternion_poses() {
    let msg = decode(&euler(&timing())).expect("euler");
    assert_eq!(msg.kind, Kind::Pose);
    assert_eq!(msg.source_id, "head");
    assert_eq!(msg.timing, timing());
    let expected = scene_math::pose(&[30., 20., 10.], false).unwrap();
    assert_eq!(msg.quaternion_xyzw, expected[..4]);
    assert_eq!(msg.euler_deg, expected[4..]);

    let half = std::f32::consts::FRAC_1_SQRT_2;
    let quaternion = pose_packet(
        "/posebridge/quaternion",
        &[0., half, 0., half],
        &timing(),
        b"head",
    );
    let msg = decode(&quaternion).expect("quaternion");
    assert!((msg.euler_deg[0] - 90.).abs() < 1e-3);
}

#[test]
fn rejects_malformed_poses() {
    let good = euler(&timing());
    for size in 0..good.len() {
        assert!(decode(&good[..size]).is_none(), "truncated at {size}");
    }
    let mut bad = good.clone();
    bad.push(0);
    assert!(decode(&bad).is_none(), "trailing byte");
    let mut bad = good.clone();
    bad[18] = 1; // padding after the address
    assert!(decode(&bad).is_none(), "nonzero padding");
    for value in [f32::NAN, f32::INFINITY] {
        let packet = pose_packet("/posebridge/euler", &[value, 0., 0.], &timing(), b"head");
        assert!(decode(&packet).is_none(), "nonfinite");
    }
    let degenerate = pose_packet("/posebridge/quaternion", &[0.; 4], &timing(), b"head");
    assert!(decode(&degenerate).is_none(), "degenerate quaternion");
    assert!(decode(&pose_packet("/posebridge/euler", &[0.; 3], &timing(), b"")).is_none());
    assert!(
        decode(&pose_packet(
            "/posebridge/euler",
            &[0.; 3],
            &timing(),
            &[b'x'; 257]
        ))
        .is_none()
    );
    assert!(
        decode(&pose_packet(
            "/posebridge/euler",
            &[0.; 4],
            &timing(),
            b"head"
        ))
        .is_none(),
        "tag count"
    );

    let mut t = timing();
    t.instance_id = 0;
    assert!(decode(&euler(&t)).is_none(), "zero key");
    let mut t = timing();
    t.tx_sequence = 1 << 63;
    assert!(decode(&euler(&t)).is_none(), "negative int64");
    let mut t = timing();
    t.sample_time_ms = 5;
    assert!(decode(&euler(&t)).is_none(), "absent clock with metadata");
    let mut t = timing();
    t.sample_time_kind = 1;
    assert!(decode(&euler(&t)).is_none(), "clock without epoch");
    let mut t = timing();
    t.sample_time_kind = 3;
    t.sample_clock_epoch = 1;
    assert!(decode(&euler(&t)).is_none(), "unknown clock kind");
    let mut t = timing();
    t.source_age_at_send_ns = 500_000_000;
    assert!(decode(&euler(&t)).is_none(), "expired at send");
    assert!(decode(&vec![0; MAX_PACKET + 1]).is_none(), "oversized");
}

#[test]
fn reports_protocol_mismatches() {
    let mut t = timing();
    t.protocol_version = 2;
    assert_eq!(decode(&euler(&t)).unwrap().kind, Kind::Incompatible);
    let mut bytes = Vec::new();
    text(&mut bytes, b"/posebridge/v2/euler");
    text(&mut bytes, b",f");
    assert_eq!(decode(&bytes).unwrap().kind, Kind::Incompatible);
    let json = status_json("active").replace(r#""schema":3"#, r#""schema":4"#);
    assert_eq!(
        decode(&telemetry(json.as_bytes(), false)).unwrap().kind,
        Kind::Incompatible
    );
}

#[test]
fn decodes_telemetry() {
    let packet = telemetry(status_json("active").as_bytes(), false);
    let msg = decode(&packet).expect("status");
    assert_eq!(msg.kind, Kind::Status);
    assert!(msg.source_active);
    assert_eq!(msg.reported_samples, 9);
    assert_eq!(msg.message_sequence, 7);
    assert_eq!(&packet[msg.json.clone()], status_json("active").as_bytes());
    assert!(
        !decode(&telemetry(status_json("stale").as_bytes(), false))
            .unwrap()
            .source_active
    );
    assert!(decode(&telemetry(status_json("asleep").as_bytes(), false)).is_none());

    let msg = decode(&telemetry(info_json().as_bytes(), true)).expect("info");
    assert_eq!(msg.kind, Kind::Info);
    assert_eq!(msg.timing.instance_id, 123);
    assert_eq!(msg.timing.source_session_id, 456);
    assert!(
        decode(&telemetry(info_json().as_bytes(), false)).is_none(),
        "kind/address mismatch"
    );

    let bom = [b"\xEF\xBB\xBF".as_slice(), info_json().as_bytes()].concat();
    let packet = telemetry(&bom, true);
    assert_eq!(&packet[decode(&packet).unwrap().json], bom.as_slice());
}

#[test]
fn rejects_malformed_telemetry() {
    let cases = [
        info_json().replace(
            r#""instance_id":"123","session_id":"456",
"reference_epoch""#,
            r#""instance_id":123,"session_id":"456",
"reference_epoch""#,
        ),
        info_json().replacen(r#""instance_id":"123""#, r#""instance_id":"0123""#, 1),
        info_json().replacen(r#""instance_id":"123""#, r#""instance_id":"0""#, 1),
        info_json().replacen(
            r#""instance_id":"123""#,
            r#""instance_id":"9223372036854775808""#,
            1,
        ),
        info_json().replace(
            r#""descriptor":{"source_id":"head","instance_id":"123""#,
            r#""descriptor":{"source_id":"head","instance_id":"124""#,
        ),
        info_json().replace("posebridge.yxz.v1", "posebridge.xyz.v1"),
        info_json().replace(r#""schema":3"#, r#""schema":3.0"#),
        info_json().replace(r#""schema":3"#, r#""schema":"3""#),
        info_json().replace(r#""kind":"info""#, r#""kind":1"#),
        info_json() + " x",
        info_json().replace(r#""source_id":"head""#, r#""source_id":"\u0000""#),
    ];
    for json in cases {
        assert!(
            decode(&telemetry(json.as_bytes(), true)).is_none(),
            "{json}"
        );
    }
    let mut packet = telemetry(info_json().as_bytes(), true);
    packet.extend_from_slice(&[0; 4]);
    assert!(decode(&packet).is_none(), "trailing words");
}

#[test]
fn json_depth_matches_frozen_callback() {
    let nest =
        |levels: usize, inner: &str| format!("{}{inner}{}", "[".repeat(levels), "]".repeat(levels));
    let with = |extra: String| info_json().replacen('{', &format!(r#"{{"extra":{extra},"#), 1);
    // The outer object is level 1, so `extra` nests from level 2.
    assert!(
        decode(&telemetry(with(nest(16, "")).as_bytes(), true)).is_some(),
        "empty level 17"
    );
    assert!(
        decode(&telemetry(with(nest(15, "1")).as_bytes(), true)).is_some(),
        "value at 16"
    );
    assert!(
        decode(&telemetry(with(nest(16, "1")).as_bytes(), true)).is_none(),
        "value at 17"
    );
    assert!(
        decode(&telemetry(with(nest(17, "")).as_bytes(), true)).is_none(),
        "container at 18"
    );
    assert!(
        decode(&telemetry(
            with(format!("\"{}\"", "[".repeat(40))).as_bytes(),
            true
        ))
        .is_some(),
        "brackets in strings"
    );
}

#[test]
fn validates_source_ids() {
    assert!(valid_source_id("耳机".as_bytes()));
    assert!(valid_source_id(b" a "));
    for bad in [
        b"".as_slice(),
        b"   ",
        b"\xc0\xaf",
        b"\xed\xa0\x80",
        b"\xf4\x90\x80\x80",
        b"\xc2\x85",
        b"a\x7f",
        b"a\tb",
        "\u{3000}\u{a0}".as_bytes(),
    ] {
        assert!(!valid_source_id(bad), "{bad:?}");
    }
    assert!(valid_source_id(&[b'x'; 256]));
    assert!(!valid_source_id(&[b'x'; 257]));
}

#[test]
fn source_order_follows_streams() {
    let mut order = SourceOrder::default();
    let mut t = timing();
    assert!(order.accept(&t));
    assert!(!order.accept(&t), "duplicate");
    t.source_sequence += 1;
    t.tx_sequence += 3;
    assert!(order.accept(&t));
    assert_eq!(order.last_gap, 2);
    let old = t.instance_id;
    t.instance_id = 999;
    assert!(order.accept(&t), "new instance");
    assert!(order.retired(old));
    t.instance_id = old;
    t.tx_sequence += 1;
    assert!(!order.accept(&t), "retired instance");
}

/// Deterministic mutation sweep: arbitrary bytes must never panic.
#[test]
fn mutated_datagrams_never_panic() {
    let seeds = [
        euler(&timing()),
        pose_packet(
            "/posebridge/quaternion",
            &[0., 0., 0., 1.],
            &timing(),
            b"head",
        ),
        telemetry(info_json().as_bytes(), true),
        telemetry(status_json("active").as_bytes(), false),
    ];
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for round in 0..100_000 {
        let mut packet = seeds[round % seeds.len()].clone();
        for _ in 0..=(next() % 4) {
            let at = (next() as usize) % packet.len().max(1);
            match next() % 4 {
                0 if !packet.is_empty() => packet[at] = next() as u8,
                1 => packet.truncate(at),
                2 => packet.insert(at.min(packet.len()), next() as u8),
                _ => packet.extend(std::iter::repeat_n(b'[', (next() % 64) as usize)),
            }
        }
        let _ = decode(&packet);
    }
}
