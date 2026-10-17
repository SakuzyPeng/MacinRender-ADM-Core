//! Semantic policy `mradm.semantic-policy.v1` (PCM consistency 4.4): parsing, rule matching,
//! merging and application to the render scene. Moved from the C++ `semantic_policy.cpp` with the
//! same validation order, messages, match rules and last-write-wins merge; the arithmetic is the
//! existing `mradm_dsp::semantic_policy`. JSON follows nlohmann/json through `mradm-json`.
use crate::error::{Context, Error, Result};
use crate::import::RenderScene;
use mradm_adm::Snapshot;
use mradm_dsp::semantic_policy::{
    self as numeric, PolicyBlock, PolicyDirect, PolicyDirectBlock, PolicyGain, PolicyObject,
    PolicyPosition, PolicyValue, SECTION_ABSENT, SECTION_DISABLED, SECTION_SCALED,
};
use mradm_json::Value;
use std::collections::HashMap;

pub const SCHEMA: &str = "mradm.semantic-policy.v1";

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Diffuse {
    pub enabled: Option<bool>,
    pub scale: Option<f32>,
    pub max: Option<f32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Extent {
    pub enabled: Option<bool>,
    pub scale: Option<f32>,
    pub width_scale: Option<f32>,
    pub height_scale: Option<f32>,
    pub depth_scale: Option<f32>,
    pub max: Option<f32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Divergence {
    pub enabled: Option<bool>,
    pub scale: Option<f32>,
    pub range_scale: Option<f32>,
    pub max_range: Option<f32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ChannelLock {
    pub enabled: Option<bool>,
    pub max_distance: Option<f32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Interpolation {
    pub honor_jump_position: Option<bool>,
    pub max_ms: Option<u32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Gain {
    pub scale: Option<f32>,
    pub gain_db: Option<f32>,
    pub mute: Option<bool>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Offset {
    pub azimuth: Option<f32>,
    pub elevation: Option<f32>,
    pub distance: Option<f32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Position {
    pub azimuth: Option<f32>,
    pub elevation: Option<f32>,
    pub distance: Option<f32>,
    pub offset: Option<Offset>,
    pub lock_azimuth: Option<f32>,
    pub lock_elevation: Option<f32>,
}

/// One DirectSpeakers action with its own block filter; never merged with another rule's.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DirectSpeakers {
    /// Empty: no label filter.
    pub speaker_label: String,
    pub lfe: Option<bool>,
    pub head_locked: Option<bool>,
    pub gain: Option<Gain>,
    pub position: Option<Position>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Override {
    pub head_locked: Option<bool>,
    pub diffuse: Option<Diffuse>,
    pub extent: Option<Extent>,
    pub divergence: Option<Divergence>,
    pub channel_lock: Option<ChannelLock>,
    pub interpolation: Option<Interpolation>,
    pub gain: Option<Gain>,
    pub position: Option<Position>,
    pub direct_speakers: Option<DirectSpeakers>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ObjectRule {
    pub policy: Override,
    pub id: String,
    pub name: String,
    pub name_glob: String,
    pub track_uid: String,
    pub content: String,
    pub programme: String,
    pub pack_format: String,
    pub all: Option<bool>,
    pub importance_min: Option<i32>,
    pub importance_max: Option<i32>,
    pub dialogue_id: Option<i32>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Policy {
    pub global: Option<Override>,
    pub objects: Vec<ObjectRule>,
}

/// What a rule can match on for one object.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Identity {
    pub object_id: String,
    pub object_name: String,
    pub track_uids: Vec<String>,
    pub importance: Option<i32>,
    pub dialogue_id: Option<i32>,
    pub content_ids: Vec<String>,
    pub content_names: Vec<String>,
    pub programme_ids: Vec<String>,
    pub programme_names: Vec<String>,
}

/// The merged object override, the DirectSpeakers actions in rule order, and the rule indices.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Resolved {
    pub object: Override,
    pub direct_speakers: Vec<DirectSpeakers>,
    pub matched: Vec<usize>,
}

// ---------------------------------------------------------------------------------------------
// Parsing

type Map = mradm_json::Map;

fn invalid(message: impl AsRef<str>) -> Error {
    Error::new(
        1,
        format!("invalid semantic policy: {}", message.as_ref()),
        Context::Label,
    )
}

/// The first key outside `allowed` in byte order, as `std::map` iteration found it.
fn unknown_key<'a>(map: &'a Map, allowed: &[&str]) -> Option<&'a str> {
    map.keys()
        .map(String::as_str)
        .find(|key| !allowed.contains(key))
}

fn object<'a>(value: &'a Value, what: &str) -> Result<&'a Map> {
    value
        .as_object()
        .ok_or_else(|| invalid(format!("'{what}' must be an object")))
}

fn check_keys(map: &Map, section: &str, allowed: &[&str]) -> Result<()> {
    match unknown_key(map, allowed) {
        Some(key) => Err(invalid(format!("unknown {section} field '{key}'"))),
        None => Ok(()),
    }
}

fn read_f32(map: &Map, key: &str) -> Result<Option<f32>> {
    let Some(value) = map.get(key) else {
        return Ok(None);
    };
    let value = value
        .as_f32()
        .ok_or_else(|| invalid(format!("'{key}' must be numeric")))?;
    if !value.is_finite() {
        return Err(invalid(format!("'{key}' must be finite")));
    }
    Ok(Some(value))
}

fn read_non_negative(map: &Map, key: &str) -> Result<Option<f32>> {
    let value = read_f32(map, key)?;
    if value.is_some_and(|v| v < 0.) {
        return Err(invalid(format!("'{key}' must be >= 0")));
    }
    Ok(value)
}

fn read_i32(map: &Map, key: &str) -> Result<Option<i32>> {
    map.get(key)
        .map(|value| {
            value
                .as_i32()
                .ok_or_else(|| invalid(format!("'{key}' must be numeric")))
        })
        .transpose()
}

fn read_u32(map: &Map, key: &str) -> Result<Option<u32>> {
    map.get(key)
        .map(|value| {
            value
                .as_unsigned_u32()
                .ok_or_else(|| invalid(format!("'{key}' must be an unsigned integer")))
        })
        .transpose()
}

fn read_bool(map: &Map, key: &str) -> Result<Option<bool>> {
    map.get(key)
        .map(|value| {
            value
                .as_bool()
                .ok_or_else(|| invalid(format!("'{key}' must be boolean")))
        })
        .transpose()
}

fn read_string(map: &Map, key: &str) -> Result<Option<String>> {
    map.get(key)
        .map(|value| {
            value
                .as_str()
                .map(str::to_owned)
                .ok_or_else(|| invalid(format!("'{key}' must be a string")))
        })
        .transpose()
}

fn parse_diffuse(value: &Value) -> Result<Diffuse> {
    let map = object(value, "diffuse")?;
    check_keys(map, "diffuse", &["enabled", "scale", "max"])?;
    Ok(Diffuse {
        enabled: read_bool(map, "enabled")?,
        scale: read_non_negative(map, "scale")?,
        max: read_non_negative(map, "max")?,
    })
}

fn parse_extent(value: &Value) -> Result<Extent> {
    let map = object(value, "extent")?;
    check_keys(
        map,
        "extent",
        &[
            "enabled",
            "scale",
            "width_scale",
            "height_scale",
            "depth_scale",
            "max",
        ],
    )?;
    Ok(Extent {
        enabled: read_bool(map, "enabled")?,
        scale: read_non_negative(map, "scale")?,
        width_scale: read_non_negative(map, "width_scale")?,
        height_scale: read_non_negative(map, "height_scale")?,
        depth_scale: read_non_negative(map, "depth_scale")?,
        max: read_non_negative(map, "max")?,
    })
}

fn parse_divergence(value: &Value) -> Result<Divergence> {
    let map = object(value, "divergence")?;
    check_keys(
        map,
        "divergence",
        &["enabled", "scale", "range_scale", "max_range"],
    )?;
    Ok(Divergence {
        enabled: read_bool(map, "enabled")?,
        scale: read_non_negative(map, "scale")?,
        range_scale: read_non_negative(map, "range_scale")?,
        max_range: read_non_negative(map, "max_range")?,
    })
}

fn parse_channel_lock(value: &Value) -> Result<ChannelLock> {
    let map = object(value, "channel_lock")?;
    check_keys(map, "channel_lock", &["enabled", "max_distance"])?;
    Ok(ChannelLock {
        enabled: read_bool(map, "enabled")?,
        max_distance: read_non_negative(map, "max_distance")?,
    })
}

fn parse_interpolation(value: &Value) -> Result<Interpolation> {
    let map = object(value, "interpolation")?;
    check_keys(map, "interpolation", &["honor_jump_position", "max_ms"])?;
    Ok(Interpolation {
        honor_jump_position: read_bool(map, "honor_jump_position")?,
        max_ms: read_u32(map, "max_ms")?,
    })
}

fn parse_gain(value: &Value) -> Result<Gain> {
    let map = object(value, "gain")?;
    check_keys(map, "gain", &["scale", "gain_db", "mute"])?;
    Ok(Gain {
        scale: read_non_negative(map, "scale")?,
        // Any finite dB offset.
        gain_db: read_f32(map, "gain_db")?,
        mute: read_bool(map, "mute")?,
    })
}

fn parse_position(value: &Value) -> Result<Position> {
    let map = object(value, "position")?;
    check_keys(
        map,
        "position",
        &[
            "azimuth",
            "elevation",
            "distance",
            "offset",
            "lock_azimuth",
            "lock_elevation",
        ],
    )?;
    let mut out = Position {
        azimuth: read_f32(map, "azimuth")?,
        elevation: read_f32(map, "elevation")?,
        distance: read_non_negative(map, "distance")?,
        lock_azimuth: read_f32(map, "lock_azimuth")?,
        lock_elevation: read_f32(map, "lock_elevation")?,
        offset: None,
    };
    if let Some(offset) = map.get("offset") {
        let offset = object(offset, "offset")?;
        check_keys(offset, "offset", &["azimuth", "elevation", "distance"])?;
        // Offsets may be negative.
        out.offset = Some(Offset {
            azimuth: read_f32(offset, "azimuth")?,
            elevation: read_f32(offset, "elevation")?,
            distance: read_f32(offset, "distance")?,
        });
    }
    Ok(out)
}

fn parse_direct_speakers(value: &Value) -> Result<DirectSpeakers> {
    let map = object(value, "direct_speakers")?;
    check_keys(
        map,
        "direct_speakers",
        &["speaker_label", "lfe", "head_locked", "gain", "position"],
    )?;
    Ok(DirectSpeakers {
        speaker_label: read_string(map, "speaker_label")?.unwrap_or_default(),
        lfe: read_bool(map, "lfe")?,
        head_locked: read_bool(map, "head_locked")?,
        gain: map.get("gain").map(parse_gain).transpose()?,
        position: map.get("position").map(parse_position).transpose()?,
    })
}

const SECTION_KEYS: [&str; 9] = [
    "diffuse",
    "extent",
    "divergence",
    "channel_lock",
    "interpolation",
    "gain",
    "position",
    "direct_speakers",
    "head_locked",
];

const RULE_KEYS: [&str; 11] = [
    "id",
    "name",
    "name_glob",
    "track_uid",
    "all",
    "importance_min",
    "importance_max",
    "dialogue_id",
    "content",
    "programme",
    "pack_format",
];

fn parse_override(value: &Value, rule: bool) -> Result<Override> {
    let map = value
        .as_object()
        .ok_or_else(|| invalid("policy section must be an object"))?;
    let allowed = |key: &str| SECTION_KEYS.contains(&key) || rule && RULE_KEYS.contains(&key);
    if let Some(key) = map.keys().find(|key| !allowed(key)) {
        return Err(invalid(format!("unknown policy field '{key}'")));
    }
    Ok(Override {
        head_locked: read_bool(map, "head_locked")?,
        diffuse: map.get("diffuse").map(parse_diffuse).transpose()?,
        extent: map.get("extent").map(parse_extent).transpose()?,
        divergence: map.get("divergence").map(parse_divergence).transpose()?,
        channel_lock: map
            .get("channel_lock")
            .map(parse_channel_lock)
            .transpose()?,
        interpolation: map
            .get("interpolation")
            .map(parse_interpolation)
            .transpose()?,
        gain: map.get("gain").map(parse_gain).transpose()?,
        position: map.get("position").map(parse_position).transpose()?,
        direct_speakers: map
            .get("direct_speakers")
            .map(parse_direct_speakers)
            .transpose()?,
    })
}

fn parse_rule(index: usize, item: &Value) -> Result<ObjectRule> {
    let map = item
        .as_object()
        .ok_or_else(|| invalid(format!("'objects[{index}]' must be an object")))?;
    let mut rule = ObjectRule {
        policy: parse_override(item, true)?,
        ..ObjectRule::default()
    };
    for (key, field) in [
        ("id", &mut rule.id),
        ("name", &mut rule.name),
        ("name_glob", &mut rule.name_glob),
        ("track_uid", &mut rule.track_uid),
        ("content", &mut rule.content),
        ("programme", &mut rule.programme),
        ("pack_format", &mut rule.pack_format),
    ] {
        if let Some(value) = read_string(map, key)? {
            *field = value;
        }
    }
    rule.all = read_bool(map, "all")?;
    rule.importance_min = read_i32(map, "importance_min")?;
    rule.importance_max = read_i32(map, "importance_max")?;
    rule.dialogue_id = read_i32(map, "dialogue_id")?;
    let has_match = !rule.id.is_empty()
        || !rule.name.is_empty()
        || !rule.name_glob.is_empty()
        || !rule.track_uid.is_empty()
        || !rule.content.is_empty()
        || !rule.programme.is_empty()
        || !rule.pack_format.is_empty()
        || rule.all.is_some()
        || rule.importance_min.is_some()
        || rule.importance_max.is_some()
        || rule.dialogue_id.is_some();
    if !has_match {
        return Err(invalid(format!(
            "'objects[{index}]' must define a match field"
        )));
    }
    Ok(rule)
}

/// Parses a policy document. Errors carry [`Context::Label`]: the caller's source label.
pub fn parse(text: &[u8]) -> Result<Policy> {
    let doc = mradm_json::parse(text).map_err(|e| invalid(e.to_string()))?;
    let map = doc
        .as_object()
        .ok_or_else(|| invalid("top-level value must be an object"))?;
    if let Some(key) = unknown_key(map, &["schema", "global", "objects"]) {
        return Err(invalid(format!("unknown top-level field '{key}'")));
    }
    let schema =
        read_string(map, "schema")?.ok_or_else(|| invalid("missing required field 'schema'"))?;
    if schema != SCHEMA {
        return Err(invalid(format!("unsupported schema '{schema}'")));
    }
    let mut policy = Policy {
        global: map
            .get("global")
            .map(|g| parse_override(g, false))
            .transpose()?,
        objects: Vec::new(),
    };
    if let Some(objects) = map.get("objects") {
        let items = objects
            .as_array()
            .ok_or_else(|| invalid("'objects' must be an array"))?;
        for (index, item) in items.iter().enumerate() {
            policy.objects.push(parse_rule(index, item)?);
        }
    }
    Ok(policy)
}

// ---------------------------------------------------------------------------------------------
// Merging and matching

fn merge<T: Clone>(dst: &mut Option<T>, src: &Option<T>) {
    if src.is_some() {
        dst.clone_from(src);
    }
}

fn merge_section<T: Default>(
    dst: &mut Option<T>,
    src: &Option<T>,
    leaves: impl FnOnce(&mut T, &T),
) {
    if let Some(src) = src {
        leaves(dst.get_or_insert_with(T::default), src);
    }
}

fn merge_gain(dst: &mut Gain, src: &Gain) {
    merge(&mut dst.scale, &src.scale);
    merge(&mut dst.gain_db, &src.gain_db);
    merge(&mut dst.mute, &src.mute);
}

/// Leaf by leaf, later wins. `direct_speakers` is never merged: each rule keeps its own filter.
fn merge_override(dst: &mut Override, src: &Override) {
    merge(&mut dst.head_locked, &src.head_locked);
    merge_section(&mut dst.diffuse, &src.diffuse, |d, s| {
        merge(&mut d.enabled, &s.enabled);
        merge(&mut d.scale, &s.scale);
        merge(&mut d.max, &s.max);
    });
    merge_section(&mut dst.extent, &src.extent, |d, s| {
        merge(&mut d.enabled, &s.enabled);
        merge(&mut d.scale, &s.scale);
        merge(&mut d.width_scale, &s.width_scale);
        merge(&mut d.height_scale, &s.height_scale);
        merge(&mut d.depth_scale, &s.depth_scale);
        merge(&mut d.max, &s.max);
    });
    merge_section(&mut dst.divergence, &src.divergence, |d, s| {
        merge(&mut d.enabled, &s.enabled);
        merge(&mut d.scale, &s.scale);
        merge(&mut d.range_scale, &s.range_scale);
        merge(&mut d.max_range, &s.max_range);
    });
    merge_section(&mut dst.channel_lock, &src.channel_lock, |d, s| {
        merge(&mut d.enabled, &s.enabled);
        merge(&mut d.max_distance, &s.max_distance);
    });
    merge_section(&mut dst.interpolation, &src.interpolation, |d, s| {
        merge(&mut d.honor_jump_position, &s.honor_jump_position);
        merge(&mut d.max_ms, &s.max_ms);
    });
    merge_section(&mut dst.gain, &src.gain, merge_gain);
    merge_section(&mut dst.position, &src.position, |d, s| {
        merge(&mut d.azimuth, &s.azimuth);
        merge(&mut d.elevation, &s.elevation);
        merge(&mut d.distance, &s.distance);
        merge(&mut d.lock_azimuth, &s.lock_azimuth);
        merge(&mut d.lock_elevation, &s.lock_elevation);
        merge_section(&mut d.offset, &s.offset, |d, s| {
            merge(&mut d.azimuth, &s.azimuth);
            merge(&mut d.elevation, &s.elevation);
            merge(&mut d.distance, &s.distance);
        });
    });
}

/// ASCII-only case folding on bytes.
fn lower(text: &str) -> Vec<u8> {
    text.bytes().map(|b| b.to_ascii_lowercase()).collect()
}

fn equal_ignore_case(a: &str, b: &str) -> bool {
    a.len() == b.len() && lower(a) == lower(b)
}

fn any_equal_ignore_case(values: &[String], target: &str) -> bool {
    values.iter().any(|v| equal_ignore_case(v, target))
}

/// `*` and `?` over bytes, case-insensitive; an empty pattern never matches.
fn glob_match(pattern: &str, text: &str) -> bool {
    if pattern.is_empty() {
        return false;
    }
    let (pat, s) = (lower(pattern), lower(text));
    let (mut p, mut i) = (0, 0);
    let mut star: Option<usize> = None;
    let mut matched = 0;
    while i < s.len() {
        if p < pat.len() && (pat[p] == b'?' || pat[p] == s[i]) {
            p += 1;
            i += 1;
        } else if p < pat.len() && pat[p] == b'*' {
            star = Some(p);
            p += 1;
            matched = i;
        } else if let Some(at) = star {
            p = at + 1;
            matched += 1;
            i = matched;
        } else {
            return false;
        }
    }
    while p < pat.len() && pat[p] == b'*' {
        p += 1;
    }
    p == pat.len()
}

/// Dialogue matching differs between the two C++ paths it was moved from: application compared
/// `static_cast<int>(dialogue)`, the report and warnings `std::cmp_equal(dialogue, rule)`.
#[derive(Clone, Copy)]
enum Dialogue {
    Cast(Option<i32>),
    Exact(Option<u32>),
}

struct Candidate<'a> {
    id: &'a str,
    name: &'a str,
    track_uids: &'a [String],
    importance: Option<i32>,
    dialogue: Dialogue,
    content_ids: &'a [String],
    content_names: &'a [String],
    programme_ids: &'a [String],
    programme_names: &'a [String],
}

fn rule_matches(c: &Candidate, rule: &ObjectRule) -> bool {
    if rule.all == Some(true) {
        return true;
    }
    if !rule.id.is_empty() && equal_ignore_case(c.id, &rule.id) {
        return true;
    }
    if !rule.name.is_empty() && equal_ignore_case(c.name, &rule.name) {
        return true;
    }
    if !rule.name_glob.is_empty() && glob_match(&rule.name_glob, c.name) {
        return true;
    }
    if !rule.track_uid.is_empty() && any_equal_ignore_case(c.track_uids, &rule.track_uid) {
        return true;
    }
    if (rule.importance_min.is_some() || rule.importance_max.is_some())
        && let Some(importance) = c.importance
        && importance >= rule.importance_min.unwrap_or(i32::MIN)
        && importance <= rule.importance_max.unwrap_or(i32::MAX)
    {
        return true;
    }
    if let Some(wanted) = rule.dialogue_id {
        let equal = match c.dialogue {
            Dialogue::Cast(value) => value == Some(wanted),
            Dialogue::Exact(value) => value.is_some_and(|v| i64::from(v) == i64::from(wanted)),
        };
        if equal {
            return true;
        }
    }
    if !rule.content.is_empty()
        && (any_equal_ignore_case(c.content_ids, &rule.content)
            || any_equal_ignore_case(c.content_names, &rule.content))
    {
        return true;
    }
    !rule.programme.is_empty()
        && (any_equal_ignore_case(c.programme_ids, &rule.programme)
            || any_equal_ignore_case(c.programme_names, &rule.programme))
}

impl Identity {
    fn candidate(&self) -> Candidate<'_> {
        Candidate {
            id: &self.object_id,
            name: &self.object_name,
            track_uids: &self.track_uids,
            importance: self.importance,
            dialogue: Dialogue::Cast(self.dialogue_id),
            content_ids: &self.content_ids,
            content_names: &self.content_names,
            programme_ids: &self.programme_ids,
            programme_names: &self.programme_names,
        }
    }
}

/// Global first, then each matching rule in array order.
pub fn resolve(policy: &Policy, identity: &Identity) -> Resolved {
    let mut out = Resolved::default();
    if let Some(global) = &policy.global {
        merge_override(&mut out.object, global);
        out.direct_speakers
            .extend(global.direct_speakers.iter().cloned());
    }
    let candidate = identity.candidate();
    for (index, rule) in policy.objects.iter().enumerate() {
        if !rule_matches(&candidate, rule) {
            continue;
        }
        out.matched.push(index);
        merge_override(&mut out.object, &rule.policy);
        out.direct_speakers
            .extend(rule.policy.direct_speakers.iter().cloned());
    }
    out
}

/// Content and programme membership of one object, from the scene graph.
#[derive(Clone, Debug, Default)]
pub(crate) struct Membership {
    pub content_ids: Vec<String>,
    pub content_names: Vec<String>,
    pub programme_ids: Vec<String>,
    pub programme_names: Vec<String>,
}

pub(crate) struct Graph<'a> {
    s: &'a Snapshot,
    membership: HashMap<&'a str, Membership>,
    empty: Membership,
}

impl<'a> Graph<'a> {
    pub fn new(s: &'a Snapshot) -> Self {
        let text = |id: u32| s.strings[id as usize].as_str();
        let ids = |span: mradm_adm::records::Span| &s.indices[span.offset..span.offset + span.len];
        let mut membership: HashMap<&str, Membership> = HashMap::new();
        // The first content wins for a repeated ID, as the C++ map emplace kept it.
        let mut content_by_id = HashMap::new();
        for content in &s.contents {
            content_by_id.entry(text(content.id)).or_insert(content);
            for &object in ids(content.object_ids) {
                let m = membership.entry(text(object)).or_default();
                m.content_ids.push(text(content.id).to_owned());
                m.content_names.push(text(content.name).to_owned());
            }
        }
        for programme in &s.programmes {
            for &content_id in ids(programme.content_ids) {
                let Some(content) = content_by_id.get(text(content_id)) else {
                    continue;
                };
                for &object in ids(content.object_ids) {
                    let m = membership.entry(text(object)).or_default();
                    m.programme_ids.push(text(programme.id).to_owned());
                    m.programme_names.push(text(programme.name).to_owned());
                }
            }
        }
        Self {
            s,
            membership,
            empty: Membership::default(),
        }
    }

    pub fn membership(&self, object: usize) -> &Membership {
        let id = self.s.strings[self.s.objects[object].id as usize].as_str();
        self.membership.get(id).unwrap_or(&self.empty)
    }

    pub fn track_uids(&self, object: usize) -> Vec<String> {
        let o = &self.s.objects[object];
        self.s.tracks[o.tracks.offset..o.tracks.offset + o.tracks.len]
            .iter()
            .map(|t| self.s.strings[t.uid as usize].clone())
            .collect()
    }

    pub fn identity(&self, object: usize) -> Identity {
        let o = &self.s.objects[object];
        let m = self.membership(object);
        Identity {
            object_id: self.s.strings[o.id as usize].clone(),
            object_name: self.s.strings[o.name as usize].clone(),
            track_uids: self.track_uids(object),
            importance: (o.importance_present != 0).then_some(o.importance),
            dialogue_id: (o.dialogue_present != 0).then_some(o.dialogue as i32),
            content_ids: m.content_ids.clone(),
            content_names: m.content_names.clone(),
            programme_ids: m.programme_ids.clone(),
            programme_names: m.programme_names.clone(),
        }
    }

    /// The report and warning variant of matching (`cmp_equal` for the dialogue ID).
    pub fn matches(&self, object: usize, track_uids: &[String], rule: &ObjectRule) -> bool {
        let o = &self.s.objects[object];
        let m = self.membership(object);
        let candidate = Candidate {
            id: &self.s.strings[o.id as usize],
            name: &self.s.strings[o.name as usize],
            track_uids,
            importance: (o.importance_present != 0).then_some(o.importance),
            dialogue: Dialogue::Exact((o.dialogue_present != 0).then_some(o.dialogue)),
            content_ids: &m.content_ids,
            content_names: &m.content_names,
            programme_ids: &m.programme_ids,
            programme_names: &m.programme_names,
        };
        rule_matches(&candidate, rule)
    }

    pub fn hoa_matches(&self, pack: usize, rule: &ObjectRule) -> bool {
        let h = &self.s.hoa[pack];
        rule.all == Some(true)
            || !rule.id.is_empty()
                && equal_ignore_case(&self.s.strings[h.object_id as usize], &rule.id)
            || !rule.pack_format.is_empty()
                && equal_ignore_case(&self.s.strings[h.pack_id as usize], &rule.pack_format)
    }
}

/// `objects[i]` names a rule in reports; `global` comes first when present.
pub(crate) fn matched_rule_names(
    graph: &Graph,
    object: usize,
    track_uids: &[String],
    policy: &Policy,
) -> Vec<String> {
    let mut out = Vec::new();
    if policy.global.is_some() {
        out.push("global".to_owned());
    }
    for (i, rule) in policy.objects.iter().enumerate() {
        if graph.matches(object, track_uids, rule) {
            out.push(format!("objects[{i}]"));
        }
    }
    out
}

/// HOA packs only report rules that carry gain or headLocked.
pub(crate) fn matched_hoa_rule_names(graph: &Graph, pack: usize, policy: &Policy) -> Vec<String> {
    let mut out = Vec::new();
    if policy
        .global
        .as_ref()
        .is_some_and(|g| g.gain.is_some() || g.head_locked.is_some())
    {
        out.push("global".to_owned());
    }
    for (i, rule) in policy.objects.iter().enumerate() {
        if (rule.policy.gain.is_some() || rule.policy.head_locked.is_some())
            && graph.hoa_matches(pack, rule)
        {
            out.push(format!("objects[{i}]"));
        }
    }
    out
}

fn rule_label(rule: &ObjectRule) -> String {
    for (name, value) in [
        ("id", &rule.id),
        ("track_uid", &rule.track_uid),
        ("name", &rule.name),
        ("name_glob", &rule.name_glob),
        ("content", &rule.content),
        ("programme", &rule.programme),
        ("pack_format", &rule.pack_format),
    ] {
        if !value.is_empty() {
            return format!("{name}={value}");
        }
    }
    if let Some(dialogue) = rule.dialogue_id {
        return format!("dialogue_id={dialogue}");
    }
    if rule.importance_min.is_some() || rule.importance_max.is_some() {
        let bound = |v: Option<i32>| v.map(|v| v.to_string()).unwrap_or_default();
        return format!(
            "importance=[{},{}]",
            bound(rule.importance_min),
            bound(rule.importance_max)
        );
    }
    "all".to_owned()
}

// ---------------------------------------------------------------------------------------------
// Application

fn value(v: Option<f32>) -> PolicyValue {
    PolicyValue {
        present: u32::from(v.is_some()),
        value: v.unwrap_or(0.),
    }
}

fn gain_rule(gain: &Gain) -> PolicyGain {
    PolicyGain {
        scale: value(gain.scale),
        gain_db: value(gain.gain_db),
    }
}

fn position_rule(position: &Option<Position>) -> PolicyPosition {
    let Some(p) = position else {
        return PolicyPosition::default();
    };
    let mut out = PolicyPosition {
        present: 1,
        azimuth: value(p.azimuth),
        elevation: value(p.elevation),
        distance: value(p.distance),
        lock_azimuth: value(p.lock_azimuth),
        lock_elevation: value(p.lock_elevation),
        ..PolicyPosition::default()
    };
    if let Some(offset) = &p.offset {
        out.has_offset = 1;
        out.offset_azimuth = value(offset.azimuth);
        out.offset_elevation = value(offset.elevation);
        out.offset_distance = value(offset.distance);
    }
    out
}

fn section(enabled: Option<Option<bool>>) -> u32 {
    match enabled {
        None => SECTION_ABSENT,
        Some(Some(false)) => SECTION_DISABLED,
        Some(_) => SECTION_SCALED,
    }
}

fn object_rule(policy: &Override) -> PolicyObject {
    let mut rule = PolicyObject {
        diffuse: section(policy.diffuse.as_ref().map(|d| d.enabled)),
        extent: section(policy.extent.as_ref().map(|e| e.enabled)),
        divergence: section(policy.divergence.as_ref().map(|d| d.enabled)),
        position: position_rule(&policy.position),
        ..PolicyObject::default()
    };
    if let Some(d) = &policy.diffuse {
        rule.diffuse_scale = value(d.scale);
        rule.diffuse_max = value(d.max);
    }
    if let Some(e) = &policy.extent {
        rule.extent_scale = value(e.scale);
        rule.width_scale = value(e.width_scale);
        rule.height_scale = value(e.height_scale);
        rule.depth_scale = value(e.depth_scale);
        rule.extent_max = value(e.max);
    }
    if let Some(d) = &policy.divergence {
        rule.divergence_scale = value(d.scale);
        rule.divergence_range_scale = value(d.range_scale);
        rule.divergence_max_range = value(d.max_range);
    }
    if let Some(i) = &policy.interpolation {
        rule.interpolation_max_ms = i.max_ms.unwrap_or(0);
    }
    rule
}

/// One Objects block: booleans here, arithmetic in `mradm_dsp::semantic_policy`.
fn apply_block(
    block: &mut mradm_adm::records::ObjectBlock,
    policy: &Override,
    rule: &PolicyObject,
    sample_rate: u32,
) {
    if let Some(head_locked) = policy.head_locked {
        block.head_locked = u32::from(head_locked);
    }
    if let Some(lock) = &policy.channel_lock {
        if let Some(enabled) = lock.enabled {
            block.channel_lock = u32::from(enabled);
            if !enabled {
                block.max_distance_present = 0;
                block.max_distance = 0.;
            }
        }
        if block.channel_lock != 0
            && let Some(distance) = lock.max_distance
        {
            block.max_distance_present = 1;
            block.max_distance = distance;
        }
    }
    if policy
        .interpolation
        .as_ref()
        .is_some_and(|i| i.honor_jump_position == Some(false))
    {
        block.jump = 0;
    }
    let p = block.position;
    let cartesian = p.cartesian != 0;
    let mut numeric_block = PolicyBlock {
        cartesian: p.cartesian,
        position: if cartesian {
            [p.x, p.y, p.z]
        } else {
            [p.azimuth, p.elevation, p.distance]
        },
        diffuse: block.diffuse,
        width: block.width,
        height: block.height,
        depth: block.depth,
        divergence: block.divergence,
        divergence_azimuth_range: block.azimuth_range,
        divergence_position_range: block.position_range,
        has_interpolation: block.interpolation_present,
        interpolation_samples: if block.interpolation_present != 0 {
            block.interpolation
        } else {
            0
        },
    };
    numeric::apply_object(rule, sample_rate, &mut numeric_block);
    if cartesian && numeric_block.cartesian == 0 {
        // A re-aimed Cartesian block becomes polar with the public position defaults.
        block.position = mradm_adm::records::AdmPosition {
            cartesian: 0,
            azimuth: 0.,
            elevation: 0.,
            distance: 1.,
            x: 0.,
            y: 0.,
            z: 0.,
        };
    }
    if numeric_block.cartesian == 0 {
        let [azimuth, elevation, distance] = numeric_block.position;
        block.position.azimuth = azimuth;
        block.position.elevation = elevation;
        block.position.distance = distance;
    }
    block.diffuse = numeric_block.diffuse;
    block.width = numeric_block.width;
    block.height = numeric_block.height;
    block.depth = numeric_block.depth;
    block.divergence = numeric_block.divergence;
    block.azimuth_range = numeric_block.divergence_azimuth_range;
    block.position_range = numeric_block.divergence_position_range;
    if block.interpolation_present != 0 {
        block.interpolation = numeric_block.interpolation_samples;
    }
}

/// The per-element user state the public scene carries next to the ADM records.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UserLevel {
    pub gain_multiplier: f32,
    pub mute: Option<bool>,
}

impl Default for UserLevel {
    fn default() -> Self {
        Self {
            gain_multiplier: 1.,
            mute: None,
        }
    }
}

/// Effective object state the records do not carry: `Object::mute` stays the authored value.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ObjectState {
    pub user: UserLevel,
    pub mute: bool,
}

/// Effective DirectSpeakers state: `DirectBlock::has_position` stays the authored value, which
/// gates the ADM source position.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DirectState {
    pub user: UserLevel,
    pub position_override: bool,
    pub has_position: bool,
}

/// State aligned with `Snapshot::objects` and `Snapshot::direct_blocks`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Overlay {
    pub objects: Vec<ObjectState>,
    pub direct_blocks: Vec<DirectState>,
}

impl Overlay {
    /// The neutral state of an imported snapshot.
    pub fn of(s: &Snapshot) -> Self {
        Self {
            objects: s
                .objects
                .iter()
                .map(|o| ObjectState {
                    user: UserLevel::default(),
                    mute: o.mute != 0,
                })
                .collect(),
            direct_blocks: s
                .direct_blocks
                .iter()
                .map(|b| DirectState {
                    user: UserLevel::default(),
                    position_override: false,
                    has_position: b.has_position != 0,
                })
                .collect(),
        }
    }
}

fn apply_direct(
    s: &Snapshot,
    block: &mut mradm_adm::records::DirectBlock,
    state: &mut DirectState,
    policy: &DirectSpeakers,
) {
    if !policy.speaker_label.is_empty() {
        let labels = &s.indices[block.labels.offset..block.labels.offset + block.labels.len];
        if !labels
            .iter()
            .any(|&l| equal_ignore_case(&s.strings[l as usize], &policy.speaker_label))
        {
            return;
        }
    }
    if policy
        .lfe
        .is_some_and(|lfe| lfe != (block.low_pass_present != 0))
    {
        return;
    }
    if let Some(head_locked) = policy.head_locked {
        block.head_locked = u32::from(head_locked);
    }
    let mut rule = PolicyDirect {
        position: position_rule(&policy.position),
        ..PolicyDirect::default()
    };
    if let Some(gain) = &policy.gain {
        rule.gain = gain_rule(gain);
        rule.mute = u32::from(gain.mute == Some(true));
        if gain.mute.is_some() {
            state.user.mute = gain.mute;
        }
    }
    let mut numeric_block = PolicyDirectBlock {
        gain: block.gain,
        user_gain_multiplier: state.user.gain_multiplier,
        has_position: u32::from(state.has_position),
        polar: [block.azimuth, block.elevation, block.distance],
        user_position_override: u32::from(state.position_override),
    };
    numeric::apply_direct(&rule, &mut numeric_block);
    block.gain = numeric_block.gain;
    state.user.gain_multiplier = numeric_block.user_gain_multiplier;
    state.has_position = numeric_block.has_position != 0;
    [block.azimuth, block.elevation, block.distance] = numeric_block.polar;
    state.position_override = numeric_block.user_position_override != 0;
}

/// Applies a policy in place and returns the "rule did not match" warnings.
pub fn apply(scene: &mut RenderScene, policy: &Policy) -> Vec<String> {
    let sample_rate = scene.info.sample_rate;
    let s = &mut scene.snapshot;
    let state = &mut scene.overlay;
    let mut warnings = Vec::new();
    let (identities, hoa_gain, hoa_head_locked) = {
        let graph = Graph::new(s);
        for rule in &policy.objects {
            let matched_object =
                (0..s.objects.len()).any(|o| graph.matches(o, &graph.track_uids(o), rule));
            let matched_hoa = (0..s.hoa.len()).any(|h| graph.hoa_matches(h, rule));
            if !matched_object && !matched_hoa {
                warnings.push(format!(
                    "semantic policy object rule did not match: {}",
                    rule_label(rule)
                ));
            }
        }
        let identities: Vec<_> = (0..s.objects.len()).map(|o| graph.identity(o)).collect();
        // HOA packs: global gain first, then each matching rule's (gain has no block filter).
        let hoa_gain: Vec<Option<Gain>> = (0..s.hoa.len())
            .map(|h| {
                let mut out: Option<Gain> = None;
                let rules = policy.global.iter().map(|g| (g, true)).chain(
                    policy
                        .objects
                        .iter()
                        .map(|r| (&r.policy, graph.hoa_matches(h, r))),
                );
                for (p, matched) in rules {
                    if let (Some(g), true) = (&p.gain, matched) {
                        merge_gain(out.get_or_insert_with(Gain::default), g);
                    }
                }
                out
            })
            .collect();
        let hoa_head_locked: Vec<Option<bool>> = (0..s.hoa.len())
            .map(|h| {
                let mut out = policy.global.as_ref().and_then(|g| g.head_locked);
                for rule in &policy.objects {
                    if rule.policy.head_locked.is_some() && graph.hoa_matches(h, rule) {
                        out = rule.policy.head_locked;
                    }
                }
                out
            })
            .collect();
        (identities, hoa_gain, hoa_head_locked)
    };

    for (index, identity) in identities.iter().enumerate() {
        let resolved = resolve(policy, identity);
        let object_policy = &resolved.object;
        let o = &mut s.objects[index];
        if let Some(gain) = &object_policy.gain {
            let rule = gain_rule(gain);
            o.gain = numeric::apply_gain(&rule, o.gain);
            let object_state = &mut state.objects[index];
            object_state.user.gain_multiplier =
                numeric::apply_gain(&rule, object_state.user.gain_multiplier);
            if let Some(mute) = gain.mute {
                object_state.mute = mute;
                object_state.user.mute = Some(mute);
            }
        }
        let block_rule = object_rule(object_policy);
        let tracks = s.objects[index].tracks;
        for t in tracks.offset..tracks.offset + tracks.len {
            let track = s.tracks[t];
            for b in track.blocks.offset..track.blocks.offset + track.blocks.len {
                apply_block(
                    &mut s.object_blocks[b],
                    object_policy,
                    &block_rule,
                    sample_rate,
                );
            }
            for b in track.ds_blocks.offset..track.ds_blocks.offset + track.ds_blocks.len {
                let mut block = s.direct_blocks[b];
                if let Some(head_locked) = object_policy.head_locked {
                    block.head_locked = u32::from(head_locked);
                }
                for direct in &resolved.direct_speakers {
                    apply_direct(s, &mut block, &mut state.direct_blocks[b], direct);
                }
                s.direct_blocks[b] = block;
            }
        }
    }

    for (h, (gain, head_locked)) in hoa_gain.iter().zip(&hoa_head_locked).enumerate() {
        let pack = &mut s.hoa[h];
        if let Some(gain) = gain {
            if let Some(mute) = gain.mute {
                pack.mute = u32::from(mute);
            }
            pack.gain = numeric::apply_gain(&gain_rule(gain), pack.gain);
        }
        if let Some(head_locked) = *head_locked {
            let channels = pack.channels;
            for c in &s.hoa_channels[channels.offset..channels.offset + channels.len] {
                for block in &mut s.hoa_blocks[c.blocks.offset..c.blocks.offset + c.blocks.len] {
                    block.head_locked = u32::from(head_locked);
                }
            }
        }
    }
    warnings
}
