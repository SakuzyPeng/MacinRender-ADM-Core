use super::*;

pub(crate) fn text(bytes: &mut Vec<u8>, value: &[u8]) {
    bytes.extend_from_slice(value);
    bytes.push(0);
    while !bytes.len().is_multiple_of(4) {
        bytes.push(0);
    }
}

pub(crate) fn timing() -> Timing {
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

pub(crate) fn pose_packet(address: &str, values: &[f32], t: &Timing, source: &[u8]) -> Vec<u8> {
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

pub(crate) fn euler(t: &Timing) -> Vec<u8> {
    pose_packet("/posebridge/euler", &[30., 20., 10.], t, b"head")
}

pub(crate) fn status_json(state: &str) -> String {
    format!(
        r#"{{"schema":3,"kind":"status","source_id":"head","instance_id":"123","session_id":"456",
"reference_epoch":"1","metadata_revision":"1","message_seq":"7",
"status":{{"session_id":"456","session_samples":"9","state":"{state}"}}}}"#
    )
}

pub(crate) fn info_json() -> String {
    r#"{"schema":3,"kind":"info","source_id":"head","instance_id":"123","session_id":"456",
"reference_epoch":"1","metadata_revision":"1","message_seq":"7",
"descriptor":{"source_id":"head","instance_id":"123","session_id":"456","reference_epoch":"1",
"metadata_revision":"1","coordinate_profile":"posebridge.yxz.v1"}}"#
        .to_owned()
}

pub(crate) fn telemetry(json: &[u8], info: bool) -> Vec<u8> {
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
fn telemetry_number_limits_match_snapshot_parser() {
    let cases = [
        ("1.7976931348623157e308".to_owned(), true),
        ("1.7976931348623158e308".to_owned(), true),
        ("1.79769313486231581e308".to_owned(), false),
        (format!("17976931348623157{}", "0".repeat(292)), true),
        (format!("179769313486231581{}", "0".repeat(291)), false),
    ];
    for (base, info) in [(info_json(), true), (status_json("active"), false)] {
        for (number, accepted) in &cases {
            for sign in ["", "-"] {
                let json = base.replacen('{', &format!(r#"{{"extra":{sign}{number},"#), 1);
                assert_eq!(
                    decode(&telemetry(json.as_bytes(), info)).is_some(),
                    *accepted,
                    "info={info}, extra={sign}{number}"
                );
            }
        }
    }
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

// ── R3 D3: the protocol checks of the C++ `osc_head_tracking_test` ────────────────────────────

fn decoded(bytes: &[u8]) -> Message {
    decode(bytes).expect("valid message rejected")
}

/// Euler (yaw, pitch, roll) vectors and their quaternions; scaled and negated quaternions decode
/// back to the same angles.
#[test]
fn quaternion_vectors_and_their_scaled_forms_round_trip() {
    const HALF: f64 = std::f64::consts::FRAC_1_SQRT_2;
    let vectors: [([f32; 3], [f64; 4]); 7] = [
        ([0., 0., 0.], [0., 0., 0., 1.]),
        ([90., 0., 0.], [0., HALF, 0., HALF]),
        ([0., 30., 0.], [0.258819045, 0., 0., 0.965925826]),
        ([0., 0., 30.], [0., 0., 0.258819045, 0.965925826]),
        (
            [30., 20., 10.],
            [0.189307857, 0.239298338, 0.038134576, 0.951548525],
        ),
        ([179., 0., 0.], [0., 0.999961923, 0., 0.008726535]),
        ([-179., 0., 0.], [0., -0.999961923, 0., 0.008726535]),
    ];
    for (angles, q) in vectors {
        let q = q.map(|c| c as f32);
        let pose = decoded(&pose_packet(
            "/posebridge/euler",
            &angles,
            &timing(),
            b"head",
        ));
        for (i, (&got, &want)) in pose.quaternion_xyzw.iter().zip(&q).enumerate() {
            assert!((got - want).abs() < 1e-6, "{angles:?}[{i}]");
        }
        for scale in [1f32, -1., 3.] {
            let scaled = q.map(|c| c * scale);
            let pose = decoded(&pose_packet(
                "/posebridge/quaternion",
                &scaled,
                &timing(),
                b"head",
            ));
            for i in 0..3 {
                assert!(
                    (pose.euler_deg[i] - angles[i]).abs() < 5e-4,
                    "{angles:?} x {scale}"
                );
            }
        }
    }
}

/// The C++ fixture timing with fields above 2^53 and a device clock.
fn wide_timing() -> Timing {
    Timing {
        instance_id: (1 << 55) + 1,
        source_sequence: (1 << 54) + 3,
        source_received_ns: (1 << 53) + 5,
        sample_time_kind: 1,
        sample_time_ms: 473_398_726_930,
        sample_clock_epoch: 1,
        ..timing()
    }
}

#[test]
fn int64_fields_keep_their_precision_and_every_key_is_checked() {
    let t = wide_timing();
    assert_eq!(decoded(&euler(&t)).timing, t);
    type Field = fn(&mut Timing) -> &mut u64;
    let keys: [(&str, Field); 6] = [
        ("instance", |t| &mut t.instance_id),
        ("session", |t| &mut t.source_session_id),
        ("source sequence", |t| &mut t.source_sequence),
        ("tx sequence", |t| &mut t.tx_sequence),
        ("reference epoch", |t| &mut t.reference_epoch),
        ("metadata revision", |t| &mut t.metadata_revision),
    ];
    for (name, field) in keys {
        for value in [0, u64::MAX] {
            let mut invalid = t;
            *field(&mut invalid) = value;
            assert!(decode(&euler(&invalid)).is_none(), "{name} = {value}");
        }
    }
    let mut absent = t;
    absent.sample_time_kind = 0;
    assert!(
        decode(&euler(&absent)).is_none(),
        "an absent clock with clock fields"
    );
}

#[test]
fn telemetry_rejects_nested_identity_mismatches_and_oversized_json() {
    let status = status_json("active").replace(
        r#""status":{"session_id":"456""#,
        r#""status":{"session_id":"999""#,
    );
    assert!(
        decode(&telemetry(status.as_bytes(), false)).is_none(),
        "status session"
    );
    let info = info_json().replace(
        r#""descriptor":{"source_id":"head","instance_id":"123","session_id":"456""#,
        r#""descriptor":{"source_id":"head","instance_id":"123","session_id":"999""#,
    );
    assert!(
        decode(&telemetry(info.as_bytes(), true)).is_none(),
        "descriptor session"
    );
    let extra = format!(r#"{{"extra":"{}","#, "x".repeat(8192));
    for (json, info) in [(info_json(), true), (status_json("active"), false)] {
        let oversized = json.replacen('{', &extra, 1);
        assert!(
            decode(&telemetry(oversized.as_bytes(), info)).is_none(),
            "info={info}"
        );
    }
}

/// `a` and `b` are the same rotation (a quaternion and its negation are).
fn same_rotation(a: [f32; 4], b: [f32; 4]) {
    let (mut same, mut opposite) = (0f64, 0f64);
    for (&x, &y) in a.iter().zip(&b) {
        same += (f64::from(x) - f64::from(y)).powi(2);
        opposite += (f64::from(x) + f64::from(y)).powi(2);
    }
    assert!(same.min(opposite) < 1e-11, "{a:?} vs {b:?}");
}

/// The decoded Euler angles describe the same rotation as the input, also at and near the poles.
#[test]
fn euler_outputs_preserve_the_rotation_at_the_poles() {
    let packet = |address: &str, values: &[f32]| pose_packet(address, values, &timing(), b"head");
    let quarter = [0.5, 0.5, -0.5, 0.5];
    let pole = decoded(&packet("/posebridge/quaternion", &quarter));
    same_rotation(
        quarter,
        decoded(&packet("/posebridge/euler", &pole.euler_deg)).quaternion_xyzw,
    );
    for pitch in [
        -90.001f32, -90., -89.99999, -89.9999, 89.9999, 89.99999, 90., 90.001,
    ] {
        for yaw in [-150f32, 0., 30., 70., 179.] {
            for roll in [-75f32, 0., 10., 60.] {
                let source = decoded(&packet("/posebridge/euler", &[yaw, pitch, roll]));
                let back = decoded(&packet("/posebridge/euler", &source.euler_deg));
                same_rotation(source.quaternion_xyzw, back.quaternion_xyzw);
                for scale in [1f32, -1., 3.] {
                    let q = source.quaternion_xyzw.map(|v| v * scale);
                    let result = decoded(&packet("/posebridge/quaternion", &q));
                    let restored = decoded(&packet("/posebridge/euler", &result.euler_deg));
                    same_rotation(source.quaternion_xyzw, restored.quaternion_xyzw);
                }
            }
        }
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

/// The ordering rules of one source: duplicates, transport gaps, host time and device clock
/// reversals, clock resets, absent timestamps, reconnects and retired instances.
#[test]
fn source_order_contract() {
    let mut order = SourceOrder::default();
    let mut t = Timing {
        sample_time_kind: 1,
        sample_time_ms: 100,
        sample_clock_epoch: 1,
        ..timing()
    };
    assert!(order.accept(&t) && !order.accept(&t), "duplicate pose");
    advance(&mut t);
    t.tx_sequence += 2;
    assert!(
        order.accept(&t) && order.last_gap == 2,
        "tx gaps are independent of sample gaps"
    );
    let mut bad = t;
    advance(&mut bad);
    bad.source_received_ns = 0;
    assert!(!order.accept(&bad), "host time reversal");
    let mut bad = t;
    advance(&mut bad);
    bad.sample_time_ms = t.sample_time_ms;
    assert!(!order.accept(&bad), "repeated device timestamp");
    bad.sample_clock_epoch += 1;
    bad.sample_time_ms = 0;
    assert!(order.accept(&bad), "explicit clock reset");
    let mut absent = bad;
    advance(&mut absent);
    absent.sample_time_kind = 0;
    absent.sample_time_ms = 0;
    absent.sample_clock_epoch = 0;
    assert!(order.accept(&absent), "absent timestamp");
    advance(&mut bad);
    bad.tx_sequence += 1;
    bad.source_sequence += 1;
    bad.sample_clock_epoch = 1;
    assert!(
        !order.accept(&bad),
        "absence does not erase the clock history"
    );
    let mut t = absent;
    advance(&mut t);
    t.source_session_id += 1;
    t.source_sequence = 1;
    t.source_received_ns = 0;
    assert!(
        !order.accept(&t),
        "a new device session needs a metadata and reference advance"
    );
    t.metadata_revision += 1;
    t.reference_epoch += 1;
    assert!(order.accept(&t), "reconnect");
    let mut next = timing();
    next.instance_id += 1;
    assert!(
        order.accept(&next) && !order.accept(&t),
        "the old instance is retired"
    );
}
