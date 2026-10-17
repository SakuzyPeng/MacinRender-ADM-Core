//! Semantic report `mradm.semantic-report.v1` and policy template (PCM consistency 4.4), moved
//! from the C++ `semantic_policy.cpp`. Byte-identical to its nlohmann `dump(2)` output: keys in
//! byte order, floats widened to double, `null` for absent values (see `mradm-json`).
use crate::error::{Context, Error, Result};
use crate::import::{RenderScene, SourceKind};
use crate::policy::{Graph, Overlay, matched_hoa_rule_names, matched_rule_names};
use mradm_adm::{Snapshot, records::*};
use mradm_json::{Map, Value};

/// The backend facts the report quotes; `renderer_effective` is the backend's own JSON object.
#[derive(Clone, Debug, Default)]
pub struct ReportOptions {
    pub renderer: String,
    pub policy_path: String,
    pub supports_channel_lock: bool,
    pub supports_object_divergence: bool,
    pub supports_diffuse: bool,
    pub hrtf_sources: Vec<String>,
    pub renderer_effective: String,
}

macro_rules! map {
    ($($key:expr => $value:expr),* $(,)?) => {{
        let mut map = Map::new();
        $(map.insert($key.to_owned(), Value::from($value));)*
        map
    }};
}

macro_rules! object {
    ($($tt:tt)*) => {
        Value::Object(map! { $($tt)* })
    };
}

fn end_sample(end: u64) -> Value {
    if end == u64::MAX {
        Value::Null
    } else {
        Value::from(end)
    }
}

fn optional<T: Into<Value>>(present: u32, value: T) -> Value {
    if present != 0 {
        value.into()
    } else {
        Value::Null
    }
}

/// Objects quote a non-finite authored gain as text; DirectSpeakers write it as a number (null).
fn object_gain_value(value: f64) -> Value {
    if value.is_finite() {
        Value::from(value)
    } else if value.is_nan() {
        Value::from("NaN")
    } else if value.is_sign_negative() {
        Value::from("-Infinity")
    } else {
        Value::from("Infinity")
    }
}

fn unit(gain: &Gain) -> &'static str {
    if gain.decibels != 0 { "dB" } else { "linear" }
}

struct View<'a> {
    s: &'a Snapshot,
    overlay: &'a Overlay,
    provenance: bool,
}

impl View<'_> {
    fn text(&self, id: u32) -> &str {
        &self.s.strings[id as usize]
    }
    fn texts(&self, span: Span) -> Vec<Value> {
        self.s.indices[span.offset..span.offset + span.len]
            .iter()
            .map(|&id| Value::from(self.text(id)))
            .collect()
    }

    fn object_block(&self, b: &ObjectBlock) -> Value {
        let p = &b.position;
        let position = if p.cartesian != 0 {
            object! { "X" => p.x, "Y" => p.y, "Z" => p.z }
        } else {
            object! { "azimuth" => p.azimuth, "elevation" => p.elevation, "distance" => p.distance }
        };
        let source = &b.source;
        let [azimuth, elevation, distance] = if p.cartesian != 0 {
            mradm_dsp::scene_math::cartesian_to_polar([p.x, p.y, p.z])
        } else {
            [p.azimuth, p.elevation, p.distance]
        };
        object! {
            "gain" => b.gain,
            "start_sample" => b.start,
            "end_sample" => end_sample(b.end),
            "cartesian" => p.cartesian != 0,
            "position" => position,
            // Objects blocks always carry their ADM source.
            "source" => object! {
                "gain_present" => source.gain.present != 0,
                "gain_unit" => unit(&source.gain),
                "gain_value" => object_gain_value(source.gain.value),
                "gain_linear" => source.gain.linear,
                "rtime_present" => source.rtime_present != 0,
                "rtime_samples" => source.rtime_samples,
                "duration_samples" => optional(source.duration_present, source.duration_samples),
            },
            "diffuse" => b.diffuse,
            "head_locked" => b.head_locked != 0,
            "width" => b.width,
            "height" => b.height,
            "depth" => b.depth,
            "divergence" => b.divergence,
            "divergence_azimuth_range" => b.azimuth_range,
            "divergence_position_range" => b.position_range,
            "channel_lock" => b.channel_lock != 0,
            "channel_lock_max_distance" => optional(b.max_distance_present, b.max_distance),
            "jump_position" => b.jump != 0,
            "interp_length_samples" => optional(b.interpolation_present, b.interpolation),
            "azimuth" => azimuth,
            "elevation" => elevation,
            "distance" => distance,
        }
    }

    fn direct_block(&self, index: usize) -> Value {
        let b = &self.s.direct_blocks[index];
        let state = &self.overlay.direct_blocks[index];
        let mut out = object! {
            "speaker_labels" => self.texts(b.labels),
            "gain" => b.gain,
            "head_locked" => b.head_locked != 0,
            "has_position" => state.has_position,
            "azimuth" => b.azimuth,
            "elevation" => b.elevation,
            "distance" => b.distance,
            "start_sample" => b.start,
            "end_sample" => end_sample(b.end),
            "low_pass_hz" => optional(b.low_pass_present, b.low_pass),
        };
        if self.provenance {
            let Value::Object(map) = &mut out else {
                unreachable!()
            };
            let source = &b.source;
            map.insert(
                "source".into(),
                object! {
                    "gain_present" => source.gain.present != 0,
                    "gain_unit" => unit(&source.gain),
                    "gain_value" => source.gain.value,
                    "gain_linear" => source.gain.linear,
                    "rtime_present" => source.rtime_present != 0,
                    "rtime_samples" => source.rtime_samples,
                    "duration_samples" => optional(source.duration_present, source.duration_samples),
                },
            );
            // The authored position, present only when the source block had one.
            if b.has_position != 0 {
                let p = &b.position;
                map.insert(
                    "source_position".into(),
                    object! {
                        "cartesian" => p.cartesian != 0,
                        "xyz" => vec![Value::from(p.x), Value::from(p.y), Value::from(p.z)],
                        "polar" => vec![Value::from(p.azimuth), Value::from(p.elevation), Value::from(p.distance)],
                    },
                );
            }
        }
        out
    }

    fn hoa_block(b: &HoaBlock) -> Value {
        object! {
            "gain" => b.gain,
            "head_locked" => b.head_locked != 0,
            "start_sample" => b.start,
            "end_sample" => end_sample(b.end),
        }
    }
}

fn invalid_effective(e: mradm_json::ParseError) -> Error {
    Error::new(6, format!("renderer semantics JSON: {e}"), Context::None)
}

/// The semantic report of a scene: original (pre-policy) against effective values.
pub fn report(scene: &RenderScene, options: &ReportOptions) -> Result<String> {
    let provenance = scene.info.source_kind == SourceKind::Adm;
    let original_overlay;
    let (original, original_overlay) = match &scene.original {
        Some((snapshot, overlay)) => (snapshot, overlay),
        None => {
            original_overlay = scene.overlay.clone();
            (&scene.snapshot, &original_overlay)
        }
    };
    let orig = View {
        s: original,
        overlay: original_overlay,
        provenance,
    };
    let eff = View {
        s: &scene.snapshot,
        overlay: &scene.overlay,
        provenance,
    };
    let graph = Graph::new(original);
    let policy = scene.policy.as_ref();

    let mut doc = Map::new();
    doc.insert("schema".into(), "mradm.semantic-report.v1".into());
    doc.insert("renderer".into(), options.renderer.as_str().into());
    doc.insert(
        "policy".into(),
        if options.policy_path.is_empty() {
            Value::Null
        } else {
            options.policy_path.as_str().into()
        },
    );
    doc.insert(
        "capabilities".into(),
        object! {
            "supports_channel_lock" => options.supports_channel_lock,
            "supports_object_divergence" => options.supports_object_divergence,
            "supports_diffuse" => options.supports_diffuse,
            "hrtf_sources" => options.hrtf_sources.iter().map(|s| Value::from(s.as_str())).collect::<Vec<_>>(),
        },
    );
    doc.insert(
        "warnings".into(),
        Value::Array(
            scene
                .policy_warnings
                .iter()
                .map(|w| w.as_str().into())
                .collect(),
        ),
    );
    if !options.renderer_effective.is_empty() {
        doc.insert(
            "renderer_effective".into(),
            mradm_json::parse(options.renderer_effective.as_bytes()).map_err(invalid_effective)?,
        );
    }

    let mut objects = Vec::new();
    for oi in 0..original.objects.len().min(scene.snapshot.objects.len()) {
        let (o, e) = (&original.objects[oi], &scene.snapshot.objects[oi]);
        let track_uids = graph.track_uids(oi);
        let mut obj = object! {
            "id" => eff.text(e.id),
            "name" => eff.text(e.name),
            "matched_rules" => policy
                .map(|p| matched_rule_names(&graph, oi, &track_uids, p))
                .unwrap_or_default()
                .into_iter()
                .map(Value::from)
                .collect::<Vec<_>>(),
            "original_gain" => o.gain,
            "effective_gain" => e.gain,
            "original_mute" => orig.overlay.objects[oi].mute,
            "effective_mute" => eff.overlay.objects[oi].mute,
            "original_head_locked" => o.head_locked != 0,
            "effective_head_locked" => e.head_locked != 0,
            "end_sample" => end_sample(e.end),
        };
        let Value::Object(map) = &mut obj else {
            unreachable!()
        };
        if provenance {
            map.insert(
                "source".into(),
                object! {
                    "gain_present" => o.source_gain.present != 0,
                    "gain_unit" => unit(&o.source_gain),
                    "gain_value" => object_gain_value(o.source_gain.value),
                    "gain_linear" => o.source_gain.linear,
                    "mute_present" => o.mute_present != 0,
                    "mute" => o.mute != 0,
                    "start_present" => o.start_present != 0,
                    "start_samples" => o.start,
                    "absolute_start_samples" => o.absolute_start,
                    "duration_samples" => optional(o.duration_present, o.duration),
                    "child_objects" => orig.texts(o.child_objects),
                    "has_parent" => o.has_parent != 0,
                },
            );
        }
        let mut blocks = Vec::new();
        for ti in 0..o.tracks.len.min(e.tracks.len) {
            let (ot, et) = (
                &original.tracks[o.tracks.offset + ti],
                &scene.snapshot.tracks[e.tracks.offset + ti],
            );
            for bi in 0..ot.blocks.len.min(et.blocks.len) {
                blocks.push(object! {
                    "kind" => "objects",
                    "track_uid" => eff.text(et.uid),
                    "pcm_channel" => if et.channel >= 0 { Value::from(et.channel as u32) } else { Value::Null },
                    "block_index" => bi,
                    "original" => orig.object_block(&original.object_blocks[ot.blocks.offset + bi]),
                    "effective" => eff.object_block(&scene.snapshot.object_blocks[et.blocks.offset + bi]),
                });
            }
            for bi in 0..ot.ds_blocks.len.min(et.ds_blocks.len) {
                blocks.push(object! {
                    "kind" => "directspeakers",
                    "track_uid" => eff.text(et.uid),
                    "block_index" => bi,
                    "original" => orig.direct_block(ot.ds_blocks.offset + bi),
                    "effective" => eff.direct_block(et.ds_blocks.offset + bi),
                });
            }
        }
        map.insert("blocks".into(), Value::Array(blocks));
        objects.push(obj);
    }
    doc.insert("objects".into(), Value::Array(objects));

    let mut packs = Vec::new();
    for hi in 0..original.hoa.len().min(scene.snapshot.hoa.len()) {
        let (o, e) = (&original.hoa[hi], &scene.snapshot.hoa[hi]);
        let mut blocks = Vec::new();
        for ci in 0..o.channels.len.min(e.channels.len) {
            let (oc, ec) = (
                &original.hoa_channels[o.channels.offset + ci],
                &scene.snapshot.hoa_channels[e.channels.offset + ci],
            );
            for bi in 0..oc.blocks.len.min(ec.blocks.len) {
                blocks.push(object! {
                    "track_uid" => eff.text(ec.uid),
                    "block_index" => bi,
                    "original" => View::hoa_block(&original.hoa_blocks[oc.blocks.offset + bi]),
                    "effective" => View::hoa_block(&scene.snapshot.hoa_blocks[ec.blocks.offset + bi]),
                });
            }
        }
        packs.push(object! {
            "object_id" => eff.text(e.object_id),
            "pack_format_id" => eff.text(e.pack_id),
            "matched_rules" => policy
                .map(|p| matched_hoa_rule_names(&graph, hi, p))
                .unwrap_or_default()
                .into_iter()
                .map(Value::from)
                .collect::<Vec<_>>(),
            "original_gain" => o.gain,
            "effective_gain" => e.gain,
            "original_mute" => o.mute != 0,
            "effective_mute" => e.mute != 0,
            "original_head_locked" => o.head_locked != 0,
            "effective_head_locked" => e.head_locked != 0,
            "blocks" => Value::Array(blocks),
        });
    }
    doc.insert("hoa_tracks".into(), Value::Array(packs));
    Ok(mradm_json::dump(&Value::Object(doc), Some(2)))
}

fn neutral_gain() -> Value {
    object! { "scale" => 1.0f32, "gain_db" => 0.0f32 }
}

/// Zero offsets only: absolute and lock keys would overwrite the position.
fn neutral_position() -> Value {
    object! { "offset" => object! { "azimuth" => 0.0f32, "elevation" => 0.0f32, "distance" => 0.0f32 } }
}

/// Applied unmodified, the template changes nothing. channel_lock, head_locked and mute have no
/// neutral value and are left out.
fn neutral_override() -> Map {
    map! {
        "diffuse" => object! { "enabled" => true, "scale" => 1.0f32, "max" => 1.0f32 },
        "extent" => object! {
            "enabled" => true,
            "scale" => 1.0f32,
            "width_scale" => 1.0f32,
            "height_scale" => 1.0f32,
            "depth_scale" => 1.0f32,
            "max" => 1.0f32,
        },
        "divergence" => object! { "enabled" => true, "scale" => 1.0f32, "range_scale" => 1.0f32, "max_range" => 120.0f32 },
        "interpolation" => object! { "honor_jump_position" => true, "max_ms" => 0u32 },
        "gain" => neutral_gain(),
        "position" => neutral_position(),
        "direct_speakers" => object! { "gain" => neutral_gain(), "position" => neutral_position() },
    }
}

/// A neutral policy with one rule per object and a lean gain-only rule per HOA pack.
pub fn template(scene: &RenderScene) -> String {
    let s = &scene.snapshot;
    let text = |id: u32| s.strings[id as usize].as_str();
    let mut objects = Vec::new();
    for o in &s.objects {
        let mut rule = neutral_override();
        rule.insert("id".into(), text(o.id).into());
        rule.insert("name".into(), text(o.name).into());
        if o.tracks.len > 0 {
            rule.insert(
                "track_uid".into(),
                text(s.tracks[o.tracks.offset].uid).into(),
            );
        }
        objects.push(Value::Object(rule));
    }
    for h in &s.hoa {
        objects.push(object! {
            "id" => text(h.object_id),
            "pack_format" => text(h.pack_id),
            "gain" => neutral_gain(),
        });
    }
    let doc = object! {
        "schema" => crate::policy::SCHEMA,
        "global" => Value::Object(neutral_override()),
        "objects" => Value::Array(objects),
    };
    mradm_json::dump(&doc, Some(2))
}
