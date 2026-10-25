//! The Triple Balance room backend (`triple-balance`, PCM consistency 4.6): the private
//! compatibility copy of the scene (file-absolute rtime timeline, native gain / mute / end
//! ignored, an initial default block), its `triple-balance-objects-v1` report, the input checks
//! and the numeric plan (bed routes, point and size tracks, D mode). Moved from the C++
//! `triple_balance_renderer`, `semantics` and `bed` with the same check order, messages and
//! report; the room numerics stay in `mradm_dsp::triple_balance`.
use crate::offline::{TbPlan, dsp_code};
use crate::routing::{self, Error, Level};
use crate::scene::{Log, in_effect, span};
use crate::vbap::Binding;
use mradm_adm::records::{DirectBlock, Object, ObjectBlock, Track};
use mradm_dsp::pcm_mix::{self, Coefficients};
use mradm_dsp::triple_balance::session::{self, TbRowInput};
use mradm_dsp::triple_balance::{Event, Layout, Position, bed, d_mode, panner};
use mradm_json::{Map, Value};
use mradm_scene::policy::UserLevel;
use mradm_scene::{RenderScene, SourceKind, layouts};
use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Standard,
    D,
}

/// `--speaker-spread-mode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Spread {
    Automatic,
    None,
    Mdap,
}

/// `--direct-speakers-routing`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Routing {
    Automatic,
    Label,
    Position,
    Matrix,
}

pub struct Request<'a> {
    pub scene: &'a RenderScene,
    pub layout_id: &'a [u8],
    pub mode: Mode,
    pub spread: Spread,
    pub split_power: bool,
    pub standard_geometry: bool,
    pub smoothing_frames: u32,
    pub routing: Routing,
    pub has_matrix: bool,
    /// The `input=` context of a channel outside the input.
    pub input_path: &'a [u8],
}

/// A track mixed through the size processor, with the scalars the live overrides check.
#[derive(Clone, Debug, PartialEq)]
pub struct SizeTrack {
    pub input_channel: u16,
    pub object_id: Vec<u8>,
    pub has_diffuse: bool,
    pub minimum_diffuse_size: f32,
    pub maximum_source_size: f32,
}

pub struct Prepared {
    pub plan: Arc<TbPlan>,
    /// One per input channel, in channel order.
    pub bindings: Vec<Binding>,
    pub size_tracks: Vec<SizeTrack>,
    pub outputs: usize,
}

/// The preparation and the backend report the semantic report quotes as `renderer_effective`,
/// written on rejection too (with its `status` and `error`).
pub struct Outcome {
    pub result: Result<Prepared, Error>,
    pub report: String,
}

const BED_LABELS: [&str; 10] = [
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts",
];
/// The 22.2 extension's report labels by output channel.
const ROOM_222_LABELS: [&str; 24] = [
    "M+060", "M-060", "M+000", "LFE1", "M+135", "M-135", "M+030", "M-030", "M+180", "LFE2",
    "M+090", "M-090", "U+045", "U-045", "U+000", "T+000", "U+135", "U-135", "U+090", "U-090",
    "U+180", "B+000", "B+045", "B-045",
];

fn error(code: i32, message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: context.into(),
    }
}

fn unsupported(message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    error(2, message, context)
}

/// A `mradm_dsp` failure as the C++ `tb_status` reported it.
fn dsp_error(e: &mradm_dsp::Error) -> Error {
    error(dsp_code(e), e.to_string(), "Triple Balance DSP")
}

fn is_room_222(layout: &[u8]) -> bool {
    layout == b"9+10+3" || layout == b"22.2"
}

pub(crate) fn tb_layout(layout: &[u8]) -> Option<Layout> {
    match layout {
        b"7.1.4" | b"4+7+0" => Some(Layout::Seven),
        b"9.1.6" => Some(Layout::Nine),
        b"22.2" | b"9+10+3" => Some(Layout::Room222),
        _ => None,
    }
}

fn obj<const N: usize>(pairs: [(&str, Value); N]) -> Value {
    Value::Object(pairs.into_iter().map(|(k, v)| (k.to_owned(), v)).collect())
}

fn floats(values: &[f32]) -> Value {
    Value::Array(values.iter().map(|&v| v.into()).collect())
}

fn ints(values: &[i64]) -> Value {
    Value::Array(values.iter().map(|&v| v.into()).collect())
}

fn decision(action: &str, source: impl Into<Value>, renderer: impl Into<Value>) -> Value {
    obj([
        ("action", action.into()),
        ("source", source.into()),
        ("renderer", renderer.into()),
    ])
}

fn optional_end(end: u64) -> Value {
    if end == u64::MAX {
        Value::Null
    } else {
        end.into()
    }
}

fn level_gain(level: &UserLevel) -> f32 {
    if level.mute.unwrap_or(false) {
        0.
    } else {
        level.gain_multiplier
    }
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

/// One track of the compatibility copy: Objects blocks on the rtime timeline, or the bed's
/// DirectSpeakers blocks (only the first is kept after the report).
struct CopyTrack<'a> {
    uid: &'a str,
    channel: Option<u16>,
    blocks: Vec<ObjectBlock>,
    /// (overlay index, block) of each DirectSpeakers block.
    direct: Vec<(usize, &'a DirectBlock)>,
}

struct CopyObject<'a> {
    id: &'a str,
    record: &'a Object,
    user: UserLevel,
    tracks: Vec<CopyTrack<'a>>,
}

struct View<'a> {
    scene: &'a RenderScene,
    provenance: bool,
}

impl<'a> View<'a> {
    fn string(&self, i: u32) -> &'a str {
        self.scene
            .snapshot
            .strings
            .get(i as usize)
            .map_or("", String::as_str)
    }

    fn labels(&self, block: &DirectBlock) -> Vec<&'a str> {
        span(&self.scene.snapshot.indices, block.labels)
            .iter()
            .map(|&i| self.string(i))
            .collect()
    }

    fn direct_user(&self, index: usize) -> &'a mradm_scene::policy::DirectState {
        &self.scene.overlay.direct_blocks[index]
    }

    /// The bed slot of a block with exactly one verified RC label.
    fn bed_channel(&self, block: &DirectBlock) -> Option<usize> {
        match self.labels(block)[..] {
            [label] => BED_LABELS.iter().position(|l| *l == label),
            _ => None,
        }
    }

    fn native_gain(&self, block: &DirectBlock) -> f32 {
        if self.provenance {
            block.source.gain.linear
        } else {
            block.gain
        }
    }

    fn bed_gains(
        &self,
        block: &DirectBlock,
        layout: &[u8],
        split_power: bool,
    ) -> Result<Vec<f32>, Error> {
        let (Some(index), Some(code)) = (self.bed_channel(block), tb_layout(layout)) else {
            return Err(unsupported(
                "room bed requires verified labels and 7.1.4/9.1.6/22.2 output",
                Vec::new(),
            ));
        };
        let mut gains = vec![0.; code.channels()];
        bed::gains(code, index, split_power, &mut gains).map_err(|e| dsp_error(&e))?;
        Ok(gains)
    }

    /// One complete 48 kHz RC 7.1.2 bed with constant labels, pack and static user gain.
    fn validate_bed(&self, object: &CopyObject) -> Result<(), Error> {
        let info = &self.scene.info;
        if info.sample_rate != 48000 || object.tracks.len() != 10 {
            return Err(unsupported(
                "triple-balance bed requires one complete 48 kHz 7.1.2 bed",
                format!("object={}; field=bed_format/sample_rate", object.id),
            ));
        }
        let mut channels = BTreeSet::new();
        let mut pack: Option<&str> = None;
        for track in &object.tracks {
            let context = format!("object={}; track={}", object.id, track.uid);
            let (Some(&(first_index, first)), true, Some(channel)) =
                (track.direct.first(), track.blocks.is_empty(), track.channel)
            else {
                return Err(unsupported(
                    "triple-balance bed requires independent DirectSpeakers tracks",
                    format!("{context}; field=track_binding"),
                ));
            };
            let index = self.bed_channel(first);
            if index.is_none_or(|i| usize::from(channel) != i || !channels.insert(i)) {
                return Err(unsupported(
                    "triple-balance requires the verified ordered RC 7.1.2 bed labels",
                    format!("{context}; field=speakerLabel/PCM"),
                ));
            }
            let pack = *pack.get_or_insert(self.string(first.pack_id));
            let level = self.direct_user(first_index).user;
            for &(i, block) in &track.direct {
                let at = format!("{context}; sample={}", block.start);
                if self.bed_channel(block) != index || self.string(block.pack_id) != pack {
                    return Err(unsupported(
                        "triple-balance bed labels and pack must remain constant",
                        format!("{at}; field=speakerLabel/pack"),
                    ));
                }
                let state = self.direct_user(i);
                let polar_source =
                    self.provenance && block.has_position != 0 && block.position.cartesian == 0;
                if block.head_locked != 0
                    || block.low_pass_present != 0
                    || block.bounds_present & 63 != 0
                    || state.position_override
                    || polar_source
                {
                    return Err(unsupported(
                        "triple-balance bed has unverified position/frequency modifiers",
                        format!("{at}; field=position/modifiers"),
                    ));
                }
                let native = self.native_gain(block);
                let user = state.user;
                if !native.is_finite()
                    || native < 0.
                    || !user.gain_multiplier.is_finite()
                    || user.gain_multiplier < 0.
                    || level.gain_multiplier != user.gain_multiplier
                    || level.mute != user.mute
                {
                    return Err(unsupported(
                        "triple-balance bed requires finite static user gain",
                        format!("{at}; field=gain"),
                    ));
                }
            }
        }
        Ok(())
    }
}

/// The 22.2 room gains of a Cartesian position.
fn room_222_gains(block: &ObjectBlock, size: f32) -> Result<panner::Gains, Error> {
    let p = &block.position;
    if p.cartesian == 0 {
        return Err(unsupported(
            "room-222 requires Cartesian coordinates",
            Vec::new(),
        ));
    }
    panner::room(
        Position {
            x: p.x,
            y: p.y,
            z: p.z,
        },
        size,
    )
    .map_err(|e| dsp_error(&e))
}

fn has_extent(b: &ObjectBlock) -> bool {
    b.width != 0. || b.height != 0. || b.depth != 0.
}

fn uses_room_size(blocks: &[ObjectBlock], spread: Spread) -> bool {
    spread != Spread::None && blocks.iter().any(has_extent)
}

/// The report document and the compatibility copy being built.
struct Semantics<'a> {
    view: View<'a>,
    request: &'a Request<'a>,
    document: Map,
}

impl<'a> Semantics<'a> {
    fn new(view: View<'a>, request: &'a Request<'a>) -> Self {
        let r = request;
        let info = &view.scene.info;
        let d = r.mode == Mode::D;
        let extended = is_room_222(r.layout_id);
        let mut document: Map = [
            ("profile", "triple-balance-objects-v1".into()),
            ("profile_version", 1i32.into()),
            ("layout", text(r.layout_id).into()),
            ("sample_rate", info.sample_rate.into()),
            ("status", "prepared".into()),
            ("time_basis", "file_absolute_samples".into()),
            ("control_frames", 512i32.into()),
            ("control_event_policy", "last_event_in_block".into()),
            (
                "event_stage",
                "targets before existing room conversion, smoothing and quantization".into(),
            ),
            ("objects", Value::Array(Vec::new())),
            ("bed_profile", "triple-balance-7.1.2-bed-v1".into()),
            ("beds", Value::Array(Vec::new())),
            ("mode", if d { "d" } else { "standard" }.into()),
        ]
        .into_iter()
        .map(|(k, v)| (k.to_owned(), v))
        .collect();
        let mut set = |k: &str, v: Value| {
            document.insert(k.to_owned(), v);
        };
        if extended {
            set("profile", "room-222-extension-v1".into());
            set("bed_profile", "source-7.1.2-to-room-222-v1".into());
            set(
                "source_semantics_profile",
                "triple-balance-objects-v1".into(),
            );
            set("spatial_reference", Value::Null);
            set(
                "spatial_model",
                "self-defined three-layer equal-power room".into(),
            );
            set(
                "size_model",
                "exact mean point power over a clipped Cartesian cube of half-width=size".into(),
            );
            set(
                "event_stage",
                "targets before float smoothing; continuous unquantized room geometry".into(),
            );
            set(
                "lower_layer_rule",
                "project Y onto the three-speaker front row".into(),
            );
            set(
                "lfe_routing",
                if r.split_power {
                    "split-power"
                } else {
                    "direct"
                }
                .into(),
            );
            let nodes = panner::NODES
                .iter()
                .map(|n| {
                    obj([
                        ("channel", n.channel.into()),
                        ("label", ROOM_222_LABELS[n.channel].into()),
                        ("xyz", floats(&[n.position.x, n.position.y, n.position.z])),
                        ("filter", if d { Value::Null } else { n.filter.into() }),
                        // The C++ `d ? 0 : sign` is a float either way.
                        ("filter_sign", if d { 0f32 } else { n.sign }.into()),
                    ])
                })
                .collect();
            set("spatial_nodes", Value::Array(nodes));
        }
        if d {
            set(
                "profile",
                if extended {
                    "triple-balance-d-222-v1"
                } else {
                    "triple-balance-d-v1"
                }
                .into(),
            );
            set("gain_control_frames", 1536i32.into());
            set("startup_ramp_frames", 1536i32.into());
            set(
                "spatial_model",
                if extended {
                    "self-defined three-layer 22.2 room with quantized position and size"
                } else {
                    "fixed 9.1.6 room geometry with quantized position and size"
                }
                .into(),
            );
            set(
                "size_model",
                "coherent kernel integration with boundary contributions and radius-based blending"
                    .into(),
            );
            set("size_decorrelation", false.into());
            set(
                "event_stage",
                "targets before room conversion, smoothing and quantization".into(),
            );
            if extended {
                set(
                    "height_rule",
                    "Z in [-1,1]; symmetric height weighting and top/bottom boundary contributions"
                        .into(),
                );
            }
        }
        Self {
            view,
            request,
            document,
        }
    }

    fn reject(&mut self, object: &str, field: &str, reason: &str, context: &str) -> Error {
        self.document.insert("status".into(), "unsupported".into());
        let mut rejection: Map = [
            ("object_id".to_owned(), object.into()),
            ("field".to_owned(), field.into()),
            ("reason".to_owned(), reason.into()),
        ]
        .into_iter()
        .collect();
        if !context.is_empty() {
            rejection.insert("context".into(), context.into());
        }
        self.document
            .insert("rejection".into(), Value::Object(rejection));
        unsupported(
            reason,
            if context.is_empty() {
                format!("object={object}; field={field}")
            } else {
                context.to_owned()
            },
        )
    }

    fn push(&mut self, key: &str, row: Value) {
        if let Some(Value::Array(rows)) = self.document.get_mut(key) {
            rows.push(row);
        }
    }

    /// Normalizes the private compatibility copy; the report is populated on rejection too.
    fn run(&mut self) -> Result<Vec<CopyObject<'a>>, Error> {
        let scene = self.view.scene;
        let s = &scene.snapshot;
        let info = &scene.info;
        let frames = info.frames;
        let r = self.request;
        let spread_none = r.spread == Spread::None;
        let mut input_channels = BTreeSet::new();
        let mut bed_count = 0usize;
        let mut objects = Vec::with_capacity(s.objects.len());
        for (oi, record) in s.objects.iter().enumerate() {
            let id = self.view.string(record.id);
            let state = &scene.overlay.objects[oi];
            if self.view.provenance && (record.has_parent != 0 || record.child_objects.len != 0) {
                return Err(self.reject(
                    id,
                    "audioObjectIDRef",
                    "triple-balance has not verified nested object semantics",
                    "",
                ));
            }
            let mut object = CopyObject {
                id,
                record,
                user: state.user,
                tracks: Vec::new(),
            };
            for track in span(&s.tracks, record.tracks) {
                let copy = copy_track(&self.view, track);
                if copy.blocks.is_empty() && copy.direct.is_empty() {
                    return Err(self.reject(
                        id,
                        "audioBlockFormat",
                        "triple-balance requires renderable metadata for each track",
                        "",
                    ));
                }
                if let Some(channel) = copy.channel
                    && !input_channels.insert(channel)
                {
                    return Err(self.reject(
                        id,
                        "audioTrackUIDRef",
                        "triple-balance requires independently bound PCM tracks",
                        "",
                    ));
                }
                object.tracks.push(copy);
            }
            let has_objects = object.tracks.iter().any(|t| !t.blocks.is_empty());
            let has_bed = object.tracks.iter().any(|t| !t.direct.is_empty());
            if has_objects && has_bed {
                return Err(self.reject(
                    id,
                    "audioPackFormat",
                    "triple-balance requires separate bed and Objects owners",
                    "",
                ));
            }
            if !has_objects {
                bed_count += 1;
                if bed_count > 1 {
                    return Err(self.reject(
                        id,
                        "audioPackFormat",
                        "triple-balance has not verified multiple beds",
                        "",
                    ));
                }
                if matches!(r.routing, Routing::Position | Routing::Matrix) || r.has_matrix {
                    return Err(self.reject(
                        id,
                        "direct_speakers_routing",
                        "triple-balance bed uses fixed reference routing",
                        "",
                    ));
                }
                if let Err(e) = self.view.validate_bed(&object) {
                    return Err(self.reject(id, "bed", &text(&e.message), &text(&e.context)));
                }
            }
            let provenance = self.view.provenance;
            let native_gain = if provenance {
                record.source_gain.linear
            } else {
                record.gain
            };
            let native_mute = if provenance {
                record.mute != 0
            } else {
                state.mute
            };
            let native_start = if provenance { record.start } else { 0 };
            let user = object.user;
            if !native_gain.is_finite()
                || native_gain < 0.
                || !user.gain_multiplier.is_finite()
                || user.gain_multiplier < 0.
            {
                return Err(self.reject(
                    id,
                    "gain",
                    "triple-balance requires finite nonnegative gain",
                    "",
                ));
            }
            if info.sample_rate != 48000
                && (native_gain != 1.
                    || native_mute
                    || native_start != 0
                    || record.end < frames
                    || user.gain_multiplier != 1.
                    || user.mute.is_some())
            {
                return Err(self.reject(
                    id,
                    "sample_rate",
                    "triple-balance extended object semantics require 48 kHz",
                    "",
                ));
            }
            let mut row: Map = [
                ("id", id.into()),
                ("native_gain", decision("ignored", native_gain, 1i32)),
                ("native_mute", decision("ignored", native_mute, false)),
                ("object_start", decision("ignored", native_start, 0i32)),
                (
                    "object_end",
                    decision("ignored", optional_end(record.end), frames),
                ),
                ("user_gain_multiplier", user.gain_multiplier.into()),
                ("user_mute", user.mute.into()),
                ("output_gain", level_gain(&user).into()),
                (
                    "output_gain_stage",
                    if has_bed {
                        "after_fixed_bed_route"
                    } else {
                        "after_object_dry_and_size_mix"
                    }
                    .into(),
                ),
                (
                    "lifecycle",
                    "PCM active throughout file; metadata durations do not gate or reset DSP"
                        .into(),
                ),
            ]
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect();
            let mut tracks = Vec::with_capacity(object.tracks.len());
            for track in &mut object.tracks {
                let row = if track.blocks.is_empty() {
                    self.bed_track(id, &user, track)?
                } else {
                    self.object_track(id, track, spread_none)?
                };
                tracks.push(row);
            }
            row.insert("tracks".into(), Value::Array(tracks));
            self.push(if has_bed { "beds" } else { "objects" }, Value::Object(row));
            objects.push(object);
        }
        Ok(objects)
    }

    fn bed_track(
        &mut self,
        id: &str,
        user: &UserLevel,
        track: &mut CopyTrack<'a>,
    ) -> Result<Value, Error> {
        let view = &self.view;
        let events = track
            .direct
            .iter()
            .map(|&(_, block)| {
                obj([
                    ("source_absolute_start_sample", block.start.into()),
                    (
                        "source_rtime_sample",
                        if view.provenance {
                            block.source.rtime_samples.into()
                        } else {
                            Value::Null
                        },
                    ),
                    ("source_end_sample", optional_end(block.end)),
                    ("gain", decision("ignored", view.native_gain(block), 1i32)),
                    ("time", "ignored; fixed route active throughout file".into()),
                ])
            })
            .collect();
        let (first_index, first) = track.direct[0];
        let gains = match view.bed_gains(first, self.request.layout_id, self.request.split_power) {
            Ok(g) => g,
            Err(e) => return Err(self.reject(id, "output_layout", &text(&e.message), "")),
        };
        let level = self.view.direct_user(first_index).user;
        let labels = self
            .view
            .labels(first)
            .into_iter()
            .map(Value::from)
            .collect();
        track.direct.truncate(1);
        Ok(obj([
            ("track_uid", track.uid.into()),
            ("pcm_channel", track.channel.map(u32::from).into()),
            ("speaker_labels", Value::Array(labels)),
            (
                "position",
                "ignored; verified bed channel identity selects routing".into(),
            ),
            ("gains_internal_layout_order", floats(&gains)),
            ("user_gain_multiplier", level.gain_multiplier.into()),
            ("user_mute", level.mute.into()),
            (
                "output_gain",
                (level_gain(user) * level_gain(&level)).into(),
            ),
            ("events", Value::Array(events)),
        ]))
    }

    fn object_track(
        &mut self,
        id: &str,
        track: &mut CopyTrack<'a>,
        spread_none: bool,
    ) -> Result<Value, Error> {
        let info = &self.view.scene.info;
        let frames = info.frames;
        let room_222 = is_room_222(self.request.layout_id) && self.request.mode == Mode::Standard;
        let mut events: Vec<Value> = Vec::with_capacity(track.blocks.len());
        let mut previous = 0u64;
        for (i, block) in track.blocks.iter_mut().enumerate() {
            let first = i == 0;
            // Object blocks always carry their ADM source.
            let relative = block.source.rtime_samples;
            if !first && relative < previous {
                return Err(self.reject(
                    id,
                    "rtime",
                    "triple-balance requires nondecreasing metadata timestamps",
                    "",
                ));
            }
            if relative >= frames {
                return Err(self.reject(
                    id,
                    "rtime",
                    "triple-balance metadata update falls outside the audio timeline",
                    "",
                ));
            }
            if !block.gain.is_finite() || block.gain < 0. {
                return Err(self.reject(
                    id,
                    "block.gain",
                    "triple-balance requires finite nonnegative block gain",
                    "",
                ));
            }
            if !first
                && relative / 512 == previous / 512
                && let Some(Value::Object(last)) = events.last_mut()
            {
                last.insert("control_target".into(), false.into());
            }
            let p = &block.position;
            let extent = [block.width, block.height, block.depth];
            let mut event: Map = [
                ("source_absolute_start_sample", block.start.into()),
                ("source_rtime_sample", relative.into()),
                ("renderer_event_sample", (relative / 512 * 512).into()),
                ("control_start_sample", (relative / 512 * 512).into()),
                ("control_target", true.into()),
                ("target_cartesian", (p.cartesian != 0).into()),
                ("target_xyz", floats(&[p.x, p.y, p.z])),
                (
                    "extent",
                    if spread_none {
                        decision("ignored_by_user", floats(&extent), ints(&[0, 0, 0]))
                    } else {
                        decision("used", floats(&extent), floats(&extent))
                    },
                ),
                ("gain", decision("ignored", block.gain, 1i32)),
                ("end", decision("ignored", optional_end(block.end), frames)),
            ]
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect();
            if room_222 {
                let size = if spread_none { 0. } else { block.width };
                let target = match room_222_gains(block, size) {
                    Ok(t) => t,
                    Err(e) => {
                        let context =
                            format!("object={id}; track={}; sample={}", track.uid, block.start);
                        return Err(self.reject(id, "position/size", &text(&e.message), &context));
                    }
                };
                let mix = panner::room_mix(target, size);
                event.insert("target_geometry_raw_gains".into(), floats(&target));
                event.insert("target_mix_direct".into(), mix.direct.into());
                event.insert("target_mix_spread_gains".into(), floats(&mix.spread));
            }
            events.push(Value::Object(event));
            block.start = relative;
            block.end = u64::MAX;
            block.gain = 1.;
            previous = relative;
        }
        let initial = track.blocks[0].start;
        if initial > 0 {
            if info.sample_rate != 48000 {
                return Err(self.reject(
                    id,
                    "first.rtime",
                    "triple-balance has not verified this initial metadata block",
                    "",
                ));
            }
            if initial < 512 {
                track.blocks[0].start = 0;
            } else {
                let mut default = ObjectBlock {
                    jump: 1,
                    ..ObjectBlock::default()
                };
                default.position.cartesian = 1;
                track.blocks.insert(0, default);
            }
        }
        let late = initial >= 512;
        Ok(obj([
            ("track_uid", track.uid.into()),
            ("pcm_channel", track.channel.map(u32::from).into()),
            (
                "initial_default_position",
                if late { ints(&[0, 0, 0]) } else { Value::Null },
            ),
            (
                "initial_default_size",
                if late { 0i32.into() } else { Value::Null },
            ),
            ("events", Value::Array(events)),
        ]))
    }

    /// The report with the outcome, as C++ `publish_semantics` wrote it.
    fn publish(mut self, error: Option<&Error>) -> String {
        if let Some(e) = error {
            self.document.insert(
                "status".into(),
                if e.code == 2 {
                    "unsupported"
                } else {
                    "rejected"
                }
                .into(),
            );
            self.document.insert(
                "error".into(),
                obj([
                    ("message", text(&e.message).into()),
                    ("context", text(&e.context).into()),
                ]),
            );
        }
        mradm_json::dump(&Value::Object(std::mem::take(&mut self.document)), None)
    }
}

fn copy_track<'a>(view: &View<'a>, track: &'a Track) -> CopyTrack<'a> {
    let s = &view.scene.snapshot;
    CopyTrack {
        uid: view.string(track.uid),
        channel: u16::try_from(track.channel).ok(),
        blocks: span(&s.object_blocks, track.blocks).to_vec(),
        direct: span(&s.direct_blocks, track.ds_blocks)
            .iter()
            .enumerate()
            .map(|(i, b)| (track.ds_blocks.offset + i, b))
            .collect(),
    }
}

/// The checks on the compatibility copy.
fn validate(view: &View, r: &Request, objects: &[CopyObject]) -> Result<(), Error> {
    let info = &view.scene.info;
    let extended = is_room_222(r.layout_id);
    let d = r.mode == Mode::D;
    if d && ((r.layout_id != b"9.1.6" && !extended) || info.sample_rate != 48000) {
        return Err(unsupported(
            "Triple Balance D mode requires 48 kHz and 9.1.6 or 22.2 output",
            Vec::new(),
        ));
    }
    if r.layout_id != b"4+7+0" && r.layout_id != b"9.1.6" && !extended {
        return Err(unsupported(
            "room renderer supports 7.1.4/9.1.6 and experimental 22.2",
            Vec::new(),
        ));
    }
    if extended && info.sample_rate != 48000 {
        return Err(unsupported(
            "experimental room-222 requires 48 kHz",
            Vec::new(),
        ));
    }
    if info.source_kind != SourceKind::Adm
        || !r.standard_geometry
        || r.spread == Spread::Mdap
        || r.smoothing_frames != 0
        || !view.scene.snapshot.hoa.is_empty()
    {
        return Err(unsupported(
            "triple-balance requires ADM input, fixed room geometry, no MDAP/extra smoothing or HOA",
            Vec::new(),
        ));
    }
    if r.routing != Routing::Automatic || r.has_matrix {
        return Err(unsupported(
            "triple-balance uses fixed bed routing; explicit routing is unsupported",
            Vec::new(),
        ));
    }
    for object in objects {
        if object.record.offset_present != 0 {
            return Err(unsupported(
                "triple-balance does not yet support positionOffset",
                format!("object={}; field=positionOffset", object.id),
            ));
        }
        for track in &object.tracks {
            if track.channel.is_none() {
                return Err(unsupported(
                    "triple-balance requires a PCM channel for each active track",
                    Vec::new(),
                ));
            }
            for block in &track.blocks {
                let extent = has_extent(block);
                if extended && let Err(e) = room_222_gains(block, 0.) {
                    return Err(error(
                        e.code,
                        e.message,
                        format!(
                            "object={}; track={}; sample={}; field=position",
                            object.id, track.uid, block.start
                        ),
                    ));
                }
                let native_extent = extent
                    && info.sample_rate == 48000
                    && block.width.is_finite()
                    && block.width >= 0.
                    && block.width <= 1.
                    && block.width == block.height
                    && block.width == block.depth
                    && (block.diffuse == 0. || block.diffuse == 1.);
                if block.position.cartesian == 0
                    || ((extent || block.diffuse != 0.)
                        && r.spread != Spread::None
                        && !native_extent)
                    || block.divergence != 0.
                    || block.channel_lock != 0
                    || block.screen_ref != 0
                    || block.head_locked != 0
                {
                    return Err(unsupported(
                        "triple-balance requires verified Cartesian point or 48 kHz equal-size Objects; \
                         spread=none explicitly ignores extent/diffuse",
                        format!(
                            "object={}; track={}; sample={}; field=position/extent/modifiers",
                            object.id, track.uid, block.start
                        ),
                    ));
                }
            }
            if uses_room_size(&track.blocks, r.spread)
                && track.blocks.last().is_none_or(|b| b.start >= info.frames)
            {
                return Err(unsupported(
                    "triple-balance size requires an object spanning the file",
                    Vec::new(),
                ));
            }
        }
    }
    Ok(())
}

/// The numeric plan of the validated copy.
fn build(
    view: &View,
    r: &Request,
    objects: &[CopyObject],
    layout: Layout,
    outputs: usize,
    log: &mut Log,
) -> Result<Prepared, Error> {
    let info = &view.scene.info;
    let inputs = usize::from(info.channels);
    let mut channels: BTreeMap<u16, (Binding, TbRowInput)> = BTreeMap::new();
    let mut events = Vec::new();
    let mut bed_coefficients = Vec::new();
    let mut size_tracks = Vec::new();
    for object in objects {
        for track in &object.tracks {
            let channel = track.channel.unwrap_or(info.channels);
            if channel >= info.channels {
                return Err(error(
                    4,
                    format!(
                        "track channel index {channel} is outside input channel count {}",
                        info.channels
                    ),
                    [b"input=".as_slice(), r.input_path].concat(),
                ));
            }
            let mut gain = level_gain(&object.user);
            let mut binding = Binding {
                input_channel: channel,
                object_id: object.id.as_bytes().to_vec(),
                label_key: Vec::new(),
            };
            let row = if let Some(&(first_index, first)) = track.direct.first() {
                let bed = view.bed_gains(first, r.layout_id, r.split_power)?;
                binding.label_key = routing::canonical_label(
                    view.labels(first).first().map_or(&[][..], |l| l.as_bytes()),
                );
                gain *= level_gain(&view.direct_user(first_index).user);
                let row = TbRowInput {
                    input: usize::from(channel),
                    event_offset: 0,
                    event_count: 0,
                    bed_offset: bed_coefficients.len(),
                    size_index: 0,
                    kind: 0,
                    gain,
                };
                bed_coefficients.extend_from_slice(&bed);
                row
            } else {
                let sized = uses_room_size(&track.blocks, r.spread);
                let offset = events.len();
                events.extend(track.blocks.iter().map(|b| Event {
                    start: b.start,
                    position: Position {
                        x: b.position.x,
                        y: b.position.y,
                        z: b.position.z,
                    },
                    size: b.width,
                }));
                let row = TbRowInput {
                    input: usize::from(channel),
                    event_offset: offset,
                    event_count: track.blocks.len(),
                    bed_offset: 0,
                    size_index: size_tracks.len(),
                    kind: if sized { 2 } else { 1 },
                    gain,
                };
                if sized {
                    let mut minimum_diffuse_size = 1f32;
                    let mut maximum_source_size = 0f32;
                    // std::max / std::min: the first argument unless the second compares beyond it.
                    for b in &track.blocks {
                        if maximum_source_size < b.width {
                            maximum_source_size = b.width;
                        }
                        if b.diffuse != 0. && b.width < minimum_diffuse_size {
                            minimum_diffuse_size = b.width;
                        }
                    }
                    size_tracks.push(SizeTrack {
                        input_channel: channel,
                        object_id: binding.object_id.clone(),
                        has_diffuse: track.blocks.iter().any(|b| b.diffuse != 0.),
                        minimum_diffuse_size,
                        maximum_source_size,
                    });
                } else if track.blocks.first().is_none_or(|b| b.start != 0) {
                    // The copy's object ends with the file.
                    return Err(unsupported(
                        "triple-balance requires an object present throughout the file",
                        Vec::new(),
                    ));
                }
                row
            };
            channels.insert(channel, (binding, row));
        }
    }
    let (bindings, rows): (Vec<_>, Vec<_>) = channels.into_values().unzip();
    let plan = match r.mode {
        Mode::D => {
            let plan = d_mode::Plan::new(
                inputs,
                layout,
                info.frames,
                &rows,
                &events,
                &bed_coefficients,
            )
            .map_err(|e| dsp_error(&e))?;
            // The D-mode processor owns all routes and clocks; the common mixer starts silent.
            let silent = pcm_mix::Plan::new(
                inputs,
                outputs,
                Vec::new(),
                Vec::new(),
                Coefficients::Speaker(Vec::new()),
            )
            .map_err(|e| dsp_error(&e))?;
            log(
                Level::Info,
                b"D mode: coherent size distribution, 512/1536-frame controls".to_vec(),
            );
            TbPlan::D {
                mix: Arc::new(silent),
                plan: Arc::new(plan),
            }
        }
        Mode::Standard => {
            let plan = session::Plan::new(
                inputs,
                layout,
                info.sample_rate,
                info.frames,
                &rows,
                &events,
                &bed_coefficients,
            )
            .map_err(|e| dsp_error(&e))?;
            if bindings.is_empty() {
                log(
                    Level::Warning,
                    b"no renderable tracks found, writing silence".to_vec(),
                );
            }
            TbPlan::Standard {
                mix: Arc::clone(&plan.mix),
                sized: !size_tracks.is_empty(),
                session: Arc::new(plan),
            }
        }
    };
    Ok(Prepared {
        plan: Arc::new(plan),
        bindings,
        size_tracks,
        outputs,
    })
}

pub fn prepare(r: &Request, log: &mut Log) -> Outcome {
    let view = View {
        scene: r.scene,
        provenance: r.scene.info.source_kind == SourceKind::Adm,
    };
    let mut semantics = Semantics::new(
        View {
            scene: r.scene,
            provenance: view.provenance,
        },
        r,
    );
    let objects = match semantics.run() {
        Ok(objects) => objects,
        Err(e) => {
            let report = semantics.publish(Some(&e));
            return Outcome {
                result: Err(e),
                report,
            };
        }
    };
    let result = prepare_copy(&view, r, &objects, log);
    let report = semantics.publish(result.as_ref().err());
    Outcome { result, report }
}

fn prepare_copy(
    view: &View,
    r: &Request,
    objects: &[CopyObject],
    log: &mut Log,
) -> Result<Prepared, Error> {
    validate(view, r, objects)?;
    let layout_id = std::str::from_utf8(r.layout_id).unwrap_or("");
    let Some(spec) = layouts::find(layout_id, layouts::Geometry::Standard) else {
        return Err(unsupported(
            "unsupported Triple Balance output layout",
            r.layout_id,
        ));
    };
    log(
        Level::Info,
        if is_room_222(r.layout_id) {
            &b"Triple Balance: experimental self-defined 22.2 extension"[..]
        } else {
            &b"Triple Balance: Cartesian Objects and verified 7.1.2 bed"[..]
        }
        .to_vec(),
    );
    let mut targets = Vec::new();
    for object in objects {
        for track in &object.tracks {
            let blocks: Vec<&DirectBlock> = track.direct.iter().map(|&(_, b)| b).collect();
            for i in in_effect(&blocks, |b| b.start) {
                let labels: Vec<&[u8]> = view
                    .labels(blocks[i])
                    .into_iter()
                    .map(str::as_bytes)
                    .collect();
                targets.push(routing::lfe_target(
                    &labels,
                    blocks[i].low_pass_present != 0,
                ));
            }
        }
    }
    routing::lfe_routing(r.split_power, r.layout_id, targets, &mut |level, text| {
        log(level, text.as_bytes().to_vec())
    })?;
    let layout = tb_layout(r.layout_id)
        .ok_or_else(|| unsupported("Unsupported Triple Balance layout", Vec::new()))?;
    build(view, r, objects, layout, spec.speakers.len(), log)
}
