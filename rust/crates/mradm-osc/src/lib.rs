//! PoseBridge protocol 3 head-tracking datagrams received over loopback OSC/UDP.
//!
//! Every byte comes from the network, so decoding is total: malformed input yields `None`
//! and never panics. One complete OSC message per datagram, at most 8 KiB. Telemetry JSON
//! and owned strings allocate, so this runs on the background receiver, never in an audio
//! callback. The accepted grammar is the frozen C++ decoder's
//! (`tests/reference/osc_protocol/`), checked field by field by a differential test.
#![forbid(unsafe_code)]

use mradm_dsp::scene_math;
use serde_json::{Map, Value};
use std::ops::Range;

pub const PROTOCOL: u32 = 3;
pub const MAX_PACKET: usize = 8192;
pub const MAX_SOURCE_ID: usize = 256;
// nlohmann::json's parse callback rejected any token below 17 open containers.
const MAX_JSON_DEPTH: usize = 17;

/// Clock domains are independent; timestamps must not be subtracted across them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Timing {
    pub protocol_version: u32,
    pub sample_time_kind: u32,
    pub instance_id: u64,
    pub tx_sequence: u64,
    pub reference_epoch: u64,
    pub metadata_revision: u64,
    pub source_age_at_send_ns: u64,
    pub source_session_id: u64,
    pub source_sequence: u64,
    pub source_received_ns: u64,
    pub sample_time_ms: u64,
    pub sample_clock_epoch: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Pose,
    Info,
    Status,
    Incompatible,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Message {
    pub kind: Kind,
    pub quaternion_xyzw: [f32; 4],
    pub euler_deg: [f32; 3],
    pub timing: Timing,
    pub source_id: String,
    pub message_sequence: u64,
    pub reported_samples: u64,
    pub source_active: bool,
    /// Telemetry JSON text as a range of the input datagram; empty otherwise.
    pub json: Range<usize>,
}

impl Message {
    fn new(kind: Kind) -> Self {
        Self {
            kind,
            quaternion_xyzw: [0., 0., 0., 1.],
            euler_deg: [0.; 3],
            timing: Timing::default(),
            source_id: String::new(),
            message_sequence: 0,
            reported_samples: 0,
            source_active: false,
            json: 0..0,
        }
    }
}

struct Reader<'a> {
    data: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    /// OSC string: NUL-terminated, zero-padded to a 4-byte boundary.
    fn string(&mut self) -> Option<Range<usize>> {
        let begin = self.offset;
        let end = begin + self.data.get(begin..)?.iter().position(|&b| b == 0)?;
        let aligned = (end + 4) & !3;
        if self.data.get(end..aligned)?.iter().any(|&b| b != 0) {
            return None;
        }
        self.offset = aligned;
        Some(begin..end)
    }
    fn integer<const N: usize>(&mut self) -> u64 {
        let bytes = &self.data[self.offset..self.offset + N];
        self.offset += N;
        bytes.iter().fold(0, |v, &b| (v << 8) | u64::from(b))
    }
    fn float(&mut self) -> f32 {
        f32::from_bits(self.integer::<4>() as u32)
    }
    fn remaining(&self) -> usize {
        self.data.len() - self.offset
    }
}

fn signed_range(value: u64) -> bool {
    value <= i64::MAX as u64
}

pub fn decode(data: &[u8]) -> Option<Message> {
    if data.len() > MAX_PACKET {
        return None;
    }
    let mut reader = Reader { data, offset: 0 };
    let address = &data[reader.string()?];
    let tags = &data[reader.string()?];
    if address.starts_with(b"/posebridge/v") {
        return Some(Message::new(Kind::Incompatible));
    }
    let info = address == b"/posebridge/info";
    if info || address == b"/posebridge/status" {
        if tags != b",s" {
            return None;
        }
        let text = reader.string()?;
        if reader.remaining() != 0 {
            return None;
        }
        return telemetry(data, text, info);
    }
    let quaternion = address == b"/posebridge/quaternion";
    if !quaternion && address != b"/posebridge/euler" {
        return None;
    }
    let expected: &[u8] = if quaternion {
        b",ishhhhhhhhihhffff"
    } else {
        b",ishhhhhhhhihhfff"
    };
    if tags != expected {
        return None;
    }
    pose(reader, quaternion)
}

fn pose(mut reader: Reader<'_>, quaternion: bool) -> Option<Message> {
    if reader.remaining() < 4 {
        return None;
    }
    if reader.integer::<4>() != u64::from(PROTOCOL) {
        return Some(Message::new(Kind::Incompatible));
    }
    let source = &reader.data[reader.string()?];
    let count = if quaternion { 4 } else { 3 };
    if !valid_source_id(source) || reader.remaining() != 84 + count * 4 {
        return None;
    }
    let mut msg = Message::new(Kind::Pose);
    msg.source_id = std::str::from_utf8(source).ok()?.to_owned();
    let t = &mut msg.timing;
    t.protocol_version = PROTOCOL;
    t.instance_id = reader.integer::<8>();
    t.source_session_id = reader.integer::<8>();
    t.source_sequence = reader.integer::<8>();
    t.tx_sequence = reader.integer::<8>();
    t.reference_epoch = reader.integer::<8>();
    t.metadata_revision = reader.integer::<8>();
    t.source_received_ns = reader.integer::<8>();
    t.source_age_at_send_ns = reader.integer::<8>();
    t.sample_time_kind = reader.integer::<4>() as u32;
    t.sample_time_ms = reader.integer::<8>();
    t.sample_clock_epoch = reader.integer::<8>();
    let positive = [
        t.instance_id,
        t.source_session_id,
        t.source_sequence,
        t.tx_sequence,
        t.reference_epoch,
        t.metadata_revision,
    ];
    let nonnegative = [
        t.source_received_ns,
        t.source_age_at_send_ns,
        t.sample_time_ms,
        t.sample_clock_epoch,
    ];
    if !positive.iter().all(|&v| v > 0 && signed_range(v))
        || !nonnegative.iter().all(|&v| signed_range(v))
        || t.source_age_at_send_ns >= 500_000_000
        || t.sample_time_kind > 2
        || (t.sample_time_kind == 0 && (t.sample_time_ms != 0 || t.sample_clock_epoch != 0))
        || (t.sample_time_kind != 0 && t.sample_clock_epoch == 0)
    {
        return None;
    }
    let mut input = [0f32; 4];
    for value in &mut input[..count] {
        *value = reader.float();
    }
    let result = scene_math::pose(&input[..count], quaternion).ok()?;
    msg.quaternion_xyzw.copy_from_slice(&result[..4]);
    msg.euler_deg.copy_from_slice(&result[4..]);
    Some(msg)
}

/// Rejects JSON with any token below `MAX_JSON_DEPTH` open containers, matching the frozen
/// decoder's nlohmann depth callback. Only meaningful for well-formed JSON; malformed text is
/// rejected by the parser afterwards.
fn within_depth(text: &[u8]) -> bool {
    let mut depth = 0usize;
    let mut string = false;
    let mut escape = false;
    for &b in text {
        if string {
            match (escape, b) {
                (true, _) => escape = false,
                (false, b'\\') => escape = true,
                (false, b'"') => string = false,
                _ => {}
            }
            continue;
        }
        match b {
            b' ' | b'\t' | b'\n' | b'\r' | b',' | b':' => {}
            b'}' | b']' => depth = depth.saturating_sub(1),
            _ => {
                if depth >= MAX_JSON_DEPTH {
                    return false;
                }
                match b {
                    b'{' | b'[' => depth += 1,
                    b'"' => string = true,
                    _ => {}
                }
            }
        }
    }
    true
}

/// Decimal identity field: digits only, no leading zero, within int64, nonzero if `positive`.
fn decimal(object: &Map<String, Value>, key: &str, positive: bool) -> Option<u64> {
    let text = object.get(key)?.as_str()?;
    if text.is_empty()
        || (text.len() > 1 && text.starts_with('0'))
        || !text.bytes().all(|b| b.is_ascii_digit())
    {
        return None;
    }
    let number: u64 = text.parse().ok()?;
    (signed_range(number) && !(positive && number == 0)).then_some(number)
}

fn text<'a>(object: &'a Map<String, Value>, key: &str) -> Option<&'a str> {
    object.get(key).and_then(Value::as_str)
}

const STATES: [&str; 11] = [
    "idle",
    "scanning",
    "connecting",
    "active",
    "stale",
    "reconnecting",
    "stopped",
    "failed",
    "configuring",
    "complete",
    "inspecting",
];

fn telemetry(data: &[u8], range: Range<usize>, info: bool) -> Option<Message> {
    let raw = &data[range.clone()];
    // nlohmann skips a leading UTF-8 byte-order mark; the stored text keeps it.
    let body = raw.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(raw);
    if !within_depth(body) {
        return None;
    }
    let value: Value = serde_json::from_slice(body).ok()?;
    let object = value.as_object()?;
    let schema = object.get("schema")?.as_u64()?;
    if schema != u64::from(PROTOCOL) {
        return Some(Message::new(Kind::Incompatible));
    }
    if text(object, "kind")? != if info { "info" } else { "status" } {
        return None;
    }
    let source_id = text(object, "source_id")?;
    if !valid_source_id(source_id.as_bytes()) {
        return None;
    }
    let instance = decimal(object, "instance_id", true)?;
    let session = decimal(object, "session_id", false)?;
    let revision = decimal(object, "metadata_revision", true)?;
    let reference = decimal(object, "reference_epoch", true)?;
    let sequence = decimal(object, "message_seq", true)?;
    let mut msg = Message::new(if info { Kind::Info } else { Kind::Status });
    msg.source_id = source_id.to_owned();
    msg.timing = Timing {
        protocol_version: PROTOCOL,
        instance_id: instance,
        source_session_id: session,
        metadata_revision: revision,
        reference_epoch: reference,
        ..Timing::default()
    };
    msg.message_sequence = sequence;
    let payload = object
        .get(if info { "descriptor" } else { "status" })?
        .as_object()?;
    if decimal(payload, "session_id", false) != Some(session) {
        return None;
    }
    if info {
        if text(payload, "source_id")? != source_id
            || decimal(payload, "instance_id", true) != Some(instance)
            || decimal(payload, "metadata_revision", true) != Some(revision)
            || decimal(payload, "reference_epoch", true) != Some(reference)
            || text(payload, "coordinate_profile")? != "posebridge.yxz.v1"
        {
            return None;
        }
    } else {
        let state = text(payload, "state")?;
        let samples = decimal(payload, "session_samples", false)?;
        if !STATES.contains(&state) {
            return None;
        }
        msg.source_active = state == "active";
        msg.reported_samples = samples;
    }
    msg.json = range;
    Some(msg)
}

/// 1..=256 bytes of valid UTF-8 without C0/C1 controls or DEL, not entirely whitespace.
pub fn valid_source_id(value: &[u8]) -> bool {
    if value.is_empty() || value.len() > MAX_SOURCE_ID {
        return false;
    }
    let Ok(text) = std::str::from_utf8(value) else {
        return false;
    };
    let whitespace = |c: char| {
        matches!(
            c,
            '\u{20}' | '\u{a0}' | '\u{1680}' | '\u{2000}'..='\u{200a}' | '\u{2028}' | '\u{2029}'
        ) || matches!(c, '\u{202f}' | '\u{205f}' | '\u{3000}')
    };
    !text
        .chars()
        .any(|c| c < '\u{20}' || ('\u{7f}'..='\u{9f}').contains(&c))
        && !text.chars().all(whitespace)
}

/// Ordering applies to accepted poses only; telemetry cannot retire a pose stream.
#[derive(Clone, Copy, Debug, Default)]
pub struct SourceOrder {
    pub last: Timing,
    pub clock: Timing,
    pub retired: [u64; 16],
    pub retired_next: usize,
    pub last_gap: u64,
}

impl SourceOrder {
    pub fn retired(&self, instance_id: u64) -> bool {
        self.retired.contains(&instance_id)
    }

    pub fn accept(&mut self, t: &Timing) -> bool {
        let last = self.last;
        let clock = self.clock;
        let new_instance = t.instance_id != last.instance_id;
        self.last_gap = 0;
        if new_instance {
            if self.retired(t.instance_id) {
                return false;
            }
        } else {
            if t.tx_sequence <= last.tx_sequence
                || t.metadata_revision < last.metadata_revision
                || t.reference_epoch < last.reference_epoch
            {
                return false;
            }
            if t.source_session_id == last.source_session_id {
                if t.source_sequence <= last.source_sequence
                    || t.source_received_ns < last.source_received_ns
                {
                    return false;
                }
                if t.sample_time_kind != 0
                    && clock.sample_time_kind != 0
                    && (t.sample_clock_epoch < clock.sample_clock_epoch
                        || (t.sample_clock_epoch == clock.sample_clock_epoch
                            && (t.sample_time_kind != clock.sample_time_kind
                                || t.sample_time_ms <= clock.sample_time_ms)))
                {
                    return false;
                }
            } else if t.metadata_revision <= last.metadata_revision
                || t.reference_epoch <= last.reference_epoch
            {
                return false;
            }
            self.last_gap = t.tx_sequence - last.tx_sequence - 1;
        }
        if new_instance {
            let slot = self.retired_next % self.retired.len();
            self.retired[slot] = last.instance_id;
            self.retired_next = (slot + 1) % self.retired.len();
        }
        if new_instance || t.source_session_id != last.source_session_id {
            self.clock = Timing::default();
        }
        self.last = *t;
        if t.sample_time_kind != 0 {
            self.clock = *t;
        }
        true
    }
}

#[cfg(test)]
mod tests;
