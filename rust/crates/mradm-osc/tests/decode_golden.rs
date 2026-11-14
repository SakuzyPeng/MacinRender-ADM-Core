//! Golden regression for the PoseBridge decoder, source-id check and source ordering.
//!
//! The corpus (seed datagrams, every prefix, pose-field sweeps, byte and JSON mutations) is the one the
//! frozen C++ differential test used before the reference was retired (user decision 2026-11-14). The
//! digests below were recorded from this implementation while it still matched that reference for every
//! input, so a change in any decision or decoded field shows up as a digest mismatch.

use mradm_osc::{Kind, SourceOrder, Timing, decode, valid_source_id};
use sha2::{Digest, Sha256};

const DECODE_DIGEST: &str = "04b6a8ca92186492f55e458d1e63648519aeebe7d5fcec82434b94f2976942c8";
const SOURCE_ID_DIGEST: &str = "b4ade8c36a4b31be0ea8e9035c3c3dd46c8edd3d2f44d49cb5dbcd97c862984d";
const SOURCE_ORDER_DIGEST: &str =
    "d3ae34e29d246598cc9d8d0b23b72c787684efda28fa1bf940eb656c60ce3bf2";
const DECODE_COMPARED: usize = 294_704;
const DECODE_ACCEPTED: usize = 5641;

struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, limit: usize) -> usize {
        if limit == 0 {
            0
        } else {
            (self.next() % limit as u64) as usize
        }
    }
}

fn append_text(bytes: &mut Vec<u8>, value: &[u8]) {
    bytes.extend_from_slice(value);
    bytes.push(0);
    while bytes.len() % 4 != 0 {
        bytes.push(0);
    }
}

fn append_integer(bytes: &mut Vec<u8>, value: u64, width: u32) {
    for shift in (0..width).rev() {
        bytes.push(((value >> (shift * 8)) & 255) as u8);
    }
}

fn fixture() -> Timing {
    Timing {
        protocol_version: 3,
        instance_id: 123,
        source_session_id: 456,
        source_sequence: 1,
        tx_sequence: 1,
        reference_epoch: 1,
        metadata_revision: 1,
        ..Timing::default()
    }
}

fn pose(quaternion: bool, t: &Timing, values: &[f32], source: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    append_text(
        &mut bytes,
        if quaternion {
            b"/posebridge/quaternion"
        } else {
            b"/posebridge/euler"
        },
    );
    append_text(
        &mut bytes,
        if quaternion {
            b",ishhhhhhhhihhffff"
        } else {
            b",ishhhhhhhhihhfff"
        },
    );
    append_integer(&mut bytes, u64::from(t.protocol_version), 4);
    append_text(&mut bytes, source);
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
        append_integer(&mut bytes, v, 8);
    }
    append_integer(&mut bytes, u64::from(t.sample_time_kind), 4);
    append_integer(&mut bytes, t.sample_time_ms, 8);
    append_integer(&mut bytes, t.sample_clock_epoch, 8);
    for v in values {
        append_integer(&mut bytes, u64::from(v.to_bits()), 4);
    }
    bytes
}

fn telemetry(json: &[u8], info: bool) -> Vec<u8> {
    let mut bytes = Vec::new();
    append_text(
        &mut bytes,
        if info {
            b"/posebridge/info"
        } else {
            b"/posebridge/status"
        },
    );
    append_text(&mut bytes, b",s");
    append_text(&mut bytes, json);
    bytes
}

const INFO: &[u8] = br#"{"schema":3,"kind":"info","source_id":"head","instance_id":"123","session_id":"456","reference_epoch":"1","metadata_revision":"1","message_seq":"7","descriptor":{"source_id":"head","instance_id":"123","session_id":"456","reference_epoch":"1","metadata_revision":"1","coordinate_profile":"posebridge.yxz.v1"}}"#;
const STATUS: &[u8] = br#"{"schema":3,"kind":"status","source_id":"head","instance_id":"123","session_id":"456","reference_epoch":"1","metadata_revision":"1","message_seq":"7","status":{"session_id":"456","session_samples":"9","state":"active"}}"#;

fn replace_first(text: &[u8], from: &[u8], to: &[u8]) -> Vec<u8> {
    match text.windows(from.len()).position(|w| w == from) {
        Some(at) => [&text[..at], to, &text[at + from.len()..]].concat(),
        None => text.to_vec(),
    }
}

fn cat(parts: &[&[u8]]) -> Vec<u8> {
    parts.concat()
}

fn json_edge_cases() -> Vec<Vec<u8>> {
    let mut cases = vec![INFO.to_vec(), STATUS.to_vec()];
    let with_extra =
        |extra: &[u8]| replace_first(INFO, b"{", &cat(&[br#"{"extra":"#, extra, b","]));
    for levels in 14..=18 {
        for inner in [&b""[..], b"1", b"{}", br#"{"a":1}"#, br#""x""#] {
            cases.push(with_extra(&cat(&[
                &vec![b'['; levels],
                inner,
                &vec![b']'; levels],
            ])));
        }
    }
    let extras: [&[u8]; 41] = [
        br#""[[[[[[[[[[[[[[[[[[[[""#,
        b"-0",
        b"1e400",
        b"-1e400",
        b"1e-400",
        b"1.7976931348623157e308",
        b"-1.7976931348623157e308",
        b"1.7976931348623158e308",
        b"-1.7976931348623158e308",
        b"1.79769313486231581e308",
        b"-1.79769313486231581e308",
        b"123456789012345678901234567890",
        b"18446744073709551615",
        b"18446744073709551616",
        b"-9223372036854775809",
        b"0.5",
        b"true",
        b"null",
        br#""\u0000""#,
        "\"😀\"".as_bytes(),
        br#""\ud800""#,
        br#""\udc00x""#,
        b"\"\x7f\"",
        b"\"\xc2\x85\"",
        b"\"\xff\"",
        b"\"\xed\xa0\x80\"",
        b"\"a\tb\"",
        br#"{"":{}}"#,
        b"[1,]",
        b"01",
        b"+1",
        b".5",
        b"1.",
        b"NaN",
        b"Infinity",
        b"/*x*/1",
        br#"["\\"]"#,
        br#""\/""#,
        br#""\x""#,
        br#""\u12""#,
        b"\"\xf0\x9f\x98\x80\"",
    ];
    for extra in extras {
        cases.push(with_extra(extra));
    }
    // The long-integer path at the same finite/overflow boundary.
    for extra in [
        cat(&[b"17976931348623157", &[b'0'; 292]]),
        cat(&[b"179769313486231581", &[b'0'; 291]]),
    ] {
        cases.push(with_extra(&extra));
        cases.push(with_extra(&cat(&[b"-", &extra])));
    }
    for schema in [
        &b"-0"[..],
        b"3.0",
        b"3e0",
        b"\"3\"",
        b"03",
        b"4",
        b"2",
        b"1e400",
        b"18446744073709551619",
    ] {
        cases.push(replace_first(
            INFO,
            br#""schema":3"#,
            &cat(&[br#""schema":"#, schema]),
        ));
    }
    cases.push(cat(&[b"\xEF\xBB\xBF", INFO]));
    cases.push(cat(&[b"\xEF\xBB", INFO]));
    cases.push(cat(&[b" \xEF\xBB\xBF", INFO]));
    cases.push(cat(&[b" \t\r\n", INFO, b" \n"]));
    cases.push(cat(&[INFO, b"x"]));
    cases.push(cat(&[INFO, INFO]));
    cases.push(replace_first(
        INFO,
        br#""schema":3,"#,
        br#""schema":3,"schema":4,"#,
    ));
    cases.push(replace_first(
        INFO,
        br#""schema":3,"#,
        br#""schema":4,"schema":3,"#,
    ));
    cases.push(replace_first(
        INFO,
        br#""kind":"info""#,
        br#""kind":"info","kind":7"#,
    ));
    cases.push(replace_first(
        INFO,
        br#""source_id":"head""#,
        br#""source_id":"head""#,
    ));
    cases.push(replace_first(
        INFO,
        br#""source_id":"head""#,
        "\"source_id\":\"\u{3000}\"".as_bytes(),
    ));
    cases.push(replace_first(
        INFO,
        br#""source_id":"head""#,
        br#""source_id":"\u0085x""#,
    ));
    cases.push(replace_first(STATUS, b"active", b"inspecting"));
    cases.push(replace_first(STATUS, b"active", b"ACTIVE"));
    cases.push(replace_first(
        STATUS,
        br#""session_samples":"9""#,
        br#""session_samples":"0""#,
    ));
    cases.push(replace_first(
        STATUS,
        br#""session_samples":"9""#,
        br#""session_samples":9"#,
    ));
    cases.push(replace_first(
        INFO,
        br#""message_seq":"7""#,
        br#""message_seq":"9223372036854775807""#,
    ));
    cases.push(replace_first(
        INFO,
        br#""message_seq":"7""#,
        br#""message_seq":"9223372036854775808""#,
    ));
    cases.push(replace_first(
        INFO,
        br#""message_seq":"7""#,
        br#""message_seq":"18446744073709551616""#,
    ));
    cases.push(b"[]".to_vec());
    cases.push(b"3".to_vec());
    cases.push(Vec::new());
    cases
}

fn seed_corpus() -> Vec<Vec<u8>> {
    let t = fixture();
    let mut seeds = Vec::new();
    for json in json_edge_cases() {
        seeds.push(telemetry(&json, true));
        seeds.push(telemetry(&json, false));
    }
    seeds.push(pose(false, &t, &[30.0, 20.0, 10.0], b"head"));
    seeds.push(pose(
        true,
        &t,
        &[0.0, 0.707_106_77, 0.0, 0.707_106_77],
        b"head",
    ));
    seeds.push(pose(true, &t, &[0.0, 0.0, 0.0, 0.0], b"head"));
    seeds.push(pose(false, &t, &[720.0, -90.0, 180.0], b"head"));
    seeds.push(pose(
        false,
        &t,
        &[f32::from_bits(0x7fc0_0001), 0.0, 0.0],
        b"head",
    ));
    seeds.push(pose(false, &t, &[0.0, 0.0, 0.0], "耳机".as_bytes()));
    let mut clock = t;
    clock.sample_time_kind = 1;
    clock.sample_clock_epoch = 1;
    clock.sample_time_ms = 1000;
    seeds.push(pose(false, &clock, &[1.0, 2.0, 3.0], b"head"));
    let mut old = t;
    old.protocol_version = 2;
    seeds.push(pose(false, &old, &[1.0, 2.0, 3.0], b"head"));
    seeds
}

fn put(hash: &mut Sha256, value: u64) {
    hash.update(value.to_le_bytes());
}

fn put_timing(hash: &mut Sha256, t: &Timing) {
    for v in [
        u64::from(t.protocol_version),
        u64::from(t.sample_time_kind),
        t.instance_id,
        t.tx_sequence,
        t.reference_epoch,
        t.metadata_revision,
        t.source_age_at_send_ns,
        t.source_session_id,
        t.source_sequence,
        t.source_received_ns,
        t.sample_time_ms,
        t.sample_clock_epoch,
    ] {
        put(hash, v);
    }
}

#[derive(Default)]
struct Decoded {
    hash: Sha256,
    compared: usize,
    accepted: usize,
}

impl Decoded {
    fn compare(&mut self, data: &[u8]) {
        self.compared += 1;
        let Some(message) = decode(data) else {
            self.hash.update([0]);
            return;
        };
        self.accepted += 1;
        let kind = match message.kind {
            Kind::Pose => 0u8,
            Kind::Info => 1,
            Kind::Status => 2,
            Kind::Incompatible => 3,
        };
        self.hash.update([1, kind]);
        for v in message.quaternion_xyzw.iter().chain(&message.euler_deg) {
            self.hash.update(v.to_bits().to_le_bytes());
        }
        put_timing(&mut self.hash, &message.timing);
        put(&mut self.hash, message.source_id.len() as u64);
        self.hash.update(message.source_id.as_bytes());
        let json = &data[message.json.clone()];
        put(&mut self.hash, json.len() as u64);
        self.hash.update(json);
        put(&mut self.hash, message.message_sequence);
        put(&mut self.hash, message.reported_samples);
        self.hash.update([u8::from(message.source_active)]);
    }
}

fn hex(digest: impl AsRef<[u8]>) -> String {
    digest.as_ref().iter().map(|b| format!("{b:02x}")).collect()
}

fn sweep_pose_fields(out: &mut Decoded, random: &mut Random) {
    let t = fixture();
    const EDGES: [u64; 9] = [
        0,
        1,
        2,
        499_999_999,
        500_000_000,
        0x7fff_ffff_ffff_ffff,
        0x8000_0000_0000_0000,
        !0,
        12345,
    ];
    for _ in 0..20_000 {
        let mut v = t;
        for field in [
            &mut v.instance_id,
            &mut v.source_session_id,
            &mut v.source_sequence,
            &mut v.tx_sequence,
            &mut v.reference_epoch,
            &mut v.metadata_revision,
            &mut v.source_received_ns,
            &mut v.source_age_at_send_ns,
            &mut v.sample_time_ms,
            &mut v.sample_clock_epoch,
        ] {
            if random.below(4) == 0 {
                *field = EDGES[random.below(EDGES.len())];
            }
        }
        v.sample_time_kind = random.below(5) as u32;
        if random.below(16) == 0 {
            v.protocol_version = random.next() as u32;
        }
        let mut values = [0.0f32; 4];
        for value in &mut values {
            *value = if random.below(8) == 0 {
                f32::from_bits(random.next() as u32)
            } else {
                (random.below(2000) as f64 - 1000.0) as f32
            };
        }
        let quaternion = random.below(2) == 0;
        out.compare(&pose(
            quaternion,
            &v,
            &values[..if quaternion { 4 } else { 3 }],
            b"head",
        ));
    }
}

fn mutate_datagrams(out: &mut Decoded, random: &mut Random, seeds: &[Vec<u8>]) {
    for _ in 0..100_000 {
        let mut packet = seeds[random.below(seeds.len())].clone();
        let edits = 1 + random.below(4);
        for _ in 0..edits {
            let at = random.below(packet.len() + 1);
            match random.below(4) {
                0 => {
                    if at < packet.len() {
                        packet[at] = random.next() as u8;
                    }
                }
                1 => packet.truncate(at),
                2 => {
                    let byte = random.next() as u8;
                    packet.insert(at, byte);
                }
                _ => {
                    if at < packet.len() {
                        packet[at] ^= 1;
                    }
                }
            }
        }
        out.compare(&packet);
    }
}

fn mutate_json(out: &mut Decoded, random: &mut Random) {
    const ALPHABET: &[u8] =
        b"{}[]\",:0123456789-+.eE \t\\/u tfnalsr\x7f\xc2\x85\xef\xbb\xbf\xed\xa0";
    let seeds = json_edge_cases();
    for _ in 0..100_000 {
        let mut json = seeds[random.below(seeds.len())].clone();
        let edits = 1 + random.below(3);
        for _ in 0..edits {
            let at = random.below(json.len() + 1);
            let c = ALPHABET[random.below(ALPHABET.len())];
            match random.below(3) {
                0 => {
                    if at < json.len() {
                        json[at] = c;
                    }
                }
                1 => {
                    if at < json.len() {
                        json.remove(at);
                    }
                }
                _ => json.insert(at, c),
            }
        }
        if !json.contains(&0) {
            let info = random.below(2) == 0;
            out.compare(&telemetry(&json, info));
        }
    }
}

#[test]
fn decode_corpus_matches_golden() {
    let mut out = Decoded::default();
    let seeds = seed_corpus();
    for seed in &seeds {
        out.compare(seed);
        for size in 0..seed.len() {
            out.compare(&seed[..size]);
        }
    }
    out.compare(&[0; 8193]);
    out.compare(&[]);
    let mut random = Random(0x5eed_0001);
    sweep_pose_fields(&mut out, &mut random);
    mutate_datagrams(&mut out, &mut random, &seeds);
    mutate_json(&mut out, &mut random);
    let (compared, accepted) = (out.compared, out.accepted);
    assert!(accepted > 1000, "corpus must exercise accepted messages");
    assert_eq!((compared, accepted), (DECODE_COMPARED, DECODE_ACCEPTED));
    assert_eq!(hex(out.hash.finalize()), DECODE_DIGEST);
}

#[test]
fn source_ids_match_golden() {
    let mut hash = Sha256::new();
    let mut check = |value: &[u8]| hash.update([u8::from(valid_source_id(value))]);
    for value in [
        &b""[..],
        b"head",
        b" ",
        b"   ",
        b"a b",
        b"\t",
        b"\x7f",
        b"\xc2\x80",
        b"\xc2\x9f",
        b"\xc2\xa0",
        b"\xc2\xa0x",
        b"\xc0\xaf",
        b"\xe0\x80\xaf",
        b"\xed\x9f\xbf",
        b"\xed\xa0\x80",
        b"\xef\xbf\xbf",
        b"\xf4\x8f\xbf\xbf",
        b"\xf4\x90\x80\x80",
        b"\xf8\x88\x80\x80\x80",
        b"\xe3\x80\x80",
        b"\xe2\x80\x8a",
        b"\xe2\x80\x8b",
        b"\xe1\x9a\x80",
        b"\xe2\x81\x9f",
        b"\xc2",
        b"\xe8\x80",
        b"\xe8\x80\xb3\xe6\x9c\xba",
    ] {
        check(value);
    }
    check(&[b'x'; 256]);
    check(&[b'x'; 257]);
    check(b"a\0b");
    let mut random = Random(0x5eed_0002);
    for _ in 0..200_000 {
        let mut value = vec![0u8; random.below(8)];
        for c in &mut value {
            let r = random.below(4);
            *c = if r == 0 {
                0x20 + random.below(0x60) as u8
            } else {
                0x80 + random.below(0x80) as u8
            };
        }
        check(&value);
    }
    assert_eq!(hex(hash.finalize()), SOURCE_ID_DIGEST);
}

#[test]
fn source_order_matches_golden() {
    let mut hash = Sha256::new();
    let mut random = Random(0x5eed_0003);
    for _ in 0..2000 {
        let mut order = SourceOrder::default();
        let mut t = fixture();
        for _ in 0..64 {
            let small = |random: &mut Random| random.below(6) as u64;
            match random.below(8) {
                0 => t.instance_id = 1 + small(&mut random),
                1 => t.source_session_id = 1 + small(&mut random),
                2 => {
                    t.sample_time_kind = random.below(3) as u32;
                    t.sample_clock_epoch = small(&mut random);
                }
                _ => {}
            }
            t.tx_sequence = if random.below(5) == 0 {
                small(&mut random)
            } else {
                t.tx_sequence + small(&mut random)
            };
            t.source_sequence = if random.below(5) == 0 {
                small(&mut random)
            } else {
                t.source_sequence + small(&mut random)
            };
            t.metadata_revision = if random.below(6) == 0 {
                small(&mut random)
            } else {
                t.metadata_revision + u64::from(random.below(4) == 0)
            };
            t.reference_epoch = if random.below(6) == 0 {
                small(&mut random)
            } else {
                t.reference_epoch + u64::from(random.below(4) == 0)
            };
            t.source_received_ns = if random.below(5) == 0 {
                small(&mut random)
            } else {
                t.source_received_ns + small(&mut random)
            };
            t.sample_time_ms = if random.below(5) == 0 {
                small(&mut random)
            } else {
                t.sample_time_ms + small(&mut random)
            };
            let accepted = order.accept(&t);
            hash.update([u8::from(accepted)]);
            put(&mut hash, order.last_gap);
            let probe = 1 + small(&mut random);
            hash.update([u8::from(order.retired(probe))]);
        }
    }
    assert_eq!(hex(hash.finalize()), SOURCE_ORDER_DIGEST);
}
