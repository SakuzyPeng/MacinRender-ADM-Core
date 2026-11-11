//! The Apple AUSpatialMixer backend's render recipe: the input buses, their
//! control events and the 22.2 LFE routing, moved from the C++ `build_bus_plans` and the Apple
//! `prepare` checks with the same rules, order, texts and arithmetic. The Apple island turns the
//! buses into AudioUnit elements and renders them; it no longer reads the C++ scene.
//!
//! Objects become point-source buses (divergence, and the extent cloud when spreading, expand to
//! parallel buses whose missing slots are silent). DirectSpeakers become an ambience-bed bus when
//! positioned, a host-side direct bus when routed by label or matrix, and a bypass bus for LFE.
//! Events are in AudioUnit convention: azimuth positive to the right, elevation clamped to
//! ±90°, distance at least 1 mm, linear gain.
use crate::routing::{
    self, Error, Level, LfeRouting, LfeTarget, MatrixRoute, ResolvedRoute, Target,
};
use crate::scene::{self, Log, in_effect, labels, span};
use mradm_adm::records::{ObjectBlock, Span};
use mradm_dsp::scene_math::{self as math, CloudPoint, Speaker};
use mradm_scene::RenderScene;
use mradm_scene::layouts::{self, Geometry};

const MODULE_WARNING: Level = Level::Warning;

/// The DirectSpeakers routing a render asks for (`adm::DirectSpeakersRoutingMode`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Routing {
    Automatic,
    Label,
    Position,
    Matrix,
}

impl Routing {
    fn name(self) -> &'static str {
        match self {
            Self::Automatic => "auto",
            Self::Label => "label",
            Self::Position => "position",
            Self::Matrix => "matrix",
        }
    }
}

/// The render options the recipe depends on. `binaural` and the layout come from the island's
/// output profile, which it resolves first.
pub struct Request<'a> {
    pub scene: &'a RenderScene,
    pub layout_id: &'a [u8],
    pub binaural: bool,
    pub routing: Routing,
    pub matrix: Option<&'a [MatrixRoute<'a>]>,
    pub lfe_split_power: bool,
    /// `adm::SpeakerSpreadMode::none`.
    pub speaker_spread_none: bool,
    /// `adm::BinauralSpreadMode::automatic` or `cloud`.
    pub binaural_spread_cloud: bool,
    pub input_path: &'a [u8],
}

/// The AUSpatialMixer source mode of a bus.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SourceMode {
    PointSource,
    AmbienceBed,
    Bypass,
}

/// One control point of a bus, already in AudioUnit convention.
#[derive(Clone, Debug, PartialEq)]
pub struct Event {
    pub start: u64,
    pub end: u64,
    pub azimuth: f32,
    pub elevation: f32,
    pub distance: f32,
    pub gain: f32,
    pub lfe: LfeTarget,
    /// Host-side destinations (output index, gain): one unity target in label mode, any number
    /// (none for an explicit mute) in matrix mode.
    pub outputs: Vec<(usize, f32)>,
    pub head_locked: bool,
}

impl Event {
    fn silent(start: u64, end: u64) -> Self {
        Self {
            start,
            end,
            azimuth: 0.,
            elevation: 0.,
            distance: 1.,
            gain: 0.,
            lfe: LfeTarget::None,
            outputs: Vec::new(),
            head_locked: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Bus {
    pub channel: u16,
    pub mode: SourceMode,
    pub is_lfe: bool,
    pub is_direct: bool,
    /// The owning object, for live gain edits.
    pub object_id: Vec<u8>,
    /// The canonical DirectSpeakers label (empty for Objects), the per-channel live gain key.
    pub label_key: Vec<u8>,
    /// Ordered by start; blocks sharing a start keep their document order.
    pub events: Vec<Event>,
}

impl Bus {
    fn new(channel: u16, mode: SourceMode, object_id: &[u8]) -> Self {
        Self {
            channel,
            mode,
            is_lfe: false,
            is_direct: false,
            object_id: object_id.to_vec(),
            label_key: Vec::new(),
            events: Vec::new(),
        }
    }
}

/// The recipe: AUSpatialMixer inputs, host-side direct inputs and 22.2 LFE side inputs.
#[derive(Clone, Debug, PartialEq)]
pub struct Plan {
    pub routing: Routing,
    pub spatial: Vec<Bus>,
    pub direct: Vec<Bus>,
    pub lfe: Vec<Bus>,
    pub lfe_routing: LfeRouting,
}

fn error(code: i32, message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: context.into(),
    }
}

fn concat(parts: &[&[u8]]) -> Vec<u8> {
    parts.concat()
}

/// SpatialMixer azimuth is positive to the right; ADM azimuth is positive to the left.
fn sm_azimuth(adm: f32) -> f32 {
    -adm
}

fn elevation(e: f32) -> f32 {
    math::clamp(e, -90., 90.)
}

fn distance(d: f32) -> f32 {
    math::max(d, 1.0e-3)
}

/// The coherent point sources of one prepared Objects block: each divergence source, further
/// expanded into the extent cloud when spreading.
fn object_events(
    sources: &[ObjectBlock],
    start: u64,
    end: u64,
    object_gain: f32,
    apply_extent: bool,
) -> Vec<Event> {
    let mut events = Vec::new();
    for source in sources {
        let [azimuth, el, d] = scene::polar(&source.position);
        let d = distance(d);
        let event = |azimuth: f32, el: f32, gain: f32| Event {
            start,
            end,
            azimuth: sm_azimuth(azimuth),
            elevation: elevation(el),
            distance: d,
            gain,
            lfe: LfeTarget::None,
            outputs: Vec::new(),
            head_locked: source.head_locked != 0,
        };
        if !apply_extent {
            events.push(event(azimuth, el, source.gain * object_gain));
            continue;
        }
        let p = &source.position;
        let mut points = [CloudPoint::default(); 17];
        let count = math::cloud(
            scene::coordinates(p),
            p.cartesian != 0,
            [source.width, source.height, source.depth],
            1.,
            false,
            false,
            &mut points,
            None,
        );
        for point in &points[..count] {
            events.push(event(
                point.azimuth,
                point.elevation,
                source.gain * object_gain * point.weight,
            ));
        }
    }
    events
}

/// Builds the recipe; `log` receives the `apple` lines in order, also before an error.
pub fn prepare(r: &Request, log: &mut Log) -> Result<Plan, Error> {
    let layout_context = || concat(&[b"layout=", r.layout_id]);
    let routing = match r.routing {
        Routing::Automatic if r.binaural => Routing::Position,
        Routing::Automatic => Routing::Label,
        other => other,
    };
    if r.binaural && routing == Routing::Label {
        return Err(error(
            2,
            "Apple binaural output does not support DirectSpeakers label routing; use position",
            layout_context(),
        ));
    }
    if r.binaural && routing == Routing::Matrix {
        return Err(error(
            2,
            "Apple binaural output does not support DirectSpeakers matrix routing",
            layout_context(),
        ));
    }
    log(
        Level::Info,
        concat(&[b"DirectSpeakers routing: ", routing.name().as_bytes()]),
    );
    let lfe = scene::lfe_routing(r.scene, r.lfe_split_power, r.layout_id, log)?;

    // Binaural none / saf_spreader and speaker none render objects as points. The cloud is a set
    // of point sources, so unlike VBAP it also spreads on 2D layouts.
    let apply_extent = if r.binaural {
        r.binaural_spread_cloud
    } else {
        !r.speaker_spread_none
    };
    // Binaural output has no discrete speakers: channelLock drops and nothing routes by label.
    let layout = if r.binaural {
        None
    } else {
        std::str::from_utf8(r.layout_id)
            .ok()
            .and_then(|id| layouts::find(id, Geometry::Apple))
    };
    let lock_speakers: Vec<Speaker> = layout.map_or_else(Vec::new, |l| {
        l.speakers
            .iter()
            .map(|s| Speaker {
                azimuth: s.azimuth,
                elevation: s.elevation,
                is_lfe: u32::from(s.is_lfe),
            })
            .collect()
    });
    let targets: Vec<Target> = layout.map_or_else(Vec::new, |l| {
        l.speakers
            .iter()
            .map(|s| Target {
                label: s.label.as_bytes(),
                is_lfe: s.is_lfe,
            })
            .collect()
    });
    let matrix: Option<Vec<ResolvedRoute>> = if routing == Routing::Matrix {
        let Some(m) = r.matrix else {
            return Err(error(
                1,
                "DirectSpeakers matrix routing requires a parsed matrix",
                Vec::new(),
            ));
        };
        Some(routing::resolve_matrix(m, &targets, r.layout_id)?)
    } else {
        None
    };

    let buses = buses(
        r,
        routing,
        apply_extent,
        &lock_speakers,
        &targets,
        matrix.as_deref(),
        log,
    )?;
    let channels = r.scene.info.channels;
    if let Some(bus) = buses.iter().find(|b| b.channel >= channels) {
        return Err(error(
            4,
            format!(
                "track channel index {} is outside input channel count {channels}",
                bus.channel
            ),
            concat(&[b"input=", r.input_path]),
        ));
    }
    let mut plan = Plan {
        routing,
        spatial: Vec::with_capacity(buses.len()),
        direct: Vec::new(),
        lfe: Vec::new(),
        lfe_routing: lfe,
    };
    for bus in buses {
        if bus.is_direct {
            plan.direct.push(bus);
        } else if lfe.applies_to_22_2 && bus.is_lfe {
            plan.lfe.push(bus);
        } else {
            plan.spatial.push(bus);
        }
    }
    if plan.spatial.is_empty() && plan.direct.is_empty() && plan.lfe.is_empty() {
        log(
            MODULE_WARNING,
            b"no renderable tracks found (all muted?), writing silence".to_vec(),
        );
    }
    if !plan.direct.is_empty() || lfe.applies_to_22_2 {
        log(
            Level::Info,
            format!(
                "routing {} spatial buses, {} direct side buses, and {} LFE side buses",
                plan.spatial.len(),
                plan.direct.len(),
                plan.lfe.len()
            )
            .into_bytes(),
        );
    }
    Ok(plan)
}

#[allow(clippy::too_many_arguments)]
fn buses(
    r: &Request,
    routing: Routing,
    apply_extent: bool,
    lock_speakers: &[Speaker],
    targets: &[Target],
    matrix: Option<&[ResolvedRoute]>,
    log: &mut Log,
) -> Result<Vec<Bus>, Error> {
    let scene = r.scene;
    let s = &scene.snapshot;
    let mut buses = Vec::new();
    let mut screen_ref_warned = false;
    for (oi, object) in s.objects.iter().enumerate() {
        if scene.overlay.objects[oi].mute {
            continue;
        }
        let object_id = s.strings[object.id as usize].as_bytes();
        for track in span(&s.tracks, object.tracks) {
            if track.channel < 0 {
                continue;
            }
            let channel = track.channel as u16;
            let blocks = span(&s.object_blocks, track.blocks);
            if !blocks.is_empty() {
                // Each block expands to a variable number of coherent point sources; one bus per
                // slot up to the most any block has, the slots a block lacks silent.
                let mut prepared = Vec::new();
                let mut slots = 1usize;
                for b in in_effect(blocks, |b| b.start) {
                    let block = scene::prepare_object_block(
                        &blocks[b],
                        object,
                        lock_speakers,
                        &mut screen_ref_warned,
                        &mut |text| log(MODULE_WARNING, text.as_bytes().to_vec()),
                    );
                    let events = object_events(
                        &block.sources,
                        block.start,
                        block.end,
                        object.gain,
                        apply_extent,
                    );
                    slots = slots.max(events.len());
                    prepared.push((block.start, block.end, events));
                }
                let mut object_buses: Vec<Bus> = (0..slots)
                    .map(|_| Bus::new(channel, SourceMode::PointSource, object_id))
                    .collect();
                for (start, end, events) in &prepared {
                    for (k, bus) in object_buses.iter_mut().enumerate() {
                        bus.events.push(
                            events
                                .get(k)
                                .cloned()
                                .unwrap_or_else(|| Event::silent(*start, *end)),
                        );
                    }
                }
                buses.extend(object_buses);
            }
            let direct = span(&s.direct_blocks, track.ds_blocks);
            if !direct.is_empty() {
                direct_buses(
                    r,
                    routing,
                    targets,
                    matrix,
                    object.gain,
                    object.end,
                    channel,
                    object_id,
                    track.ds_blocks,
                    &mut buses,
                    log,
                )?;
            }
        }
    }
    // Blocks sharing a start keep their scene (ADM document) order; the last one is in effect.
    for bus in &mut buses {
        bus.events.sort_by_key(|e| e.start);
    }
    Ok(buses)
}

/// A DirectSpeakers channel keeps at most one spatial / direct bus and one LFE bus, so LFE
/// intervals peel off without dropping non-LFE intervals on the same track.
#[allow(clippy::too_many_arguments)]
fn direct_buses(
    r: &Request,
    routing: Routing,
    targets: &[Target],
    matrix: Option<&[ResolvedRoute]>,
    object_gain: f32,
    object_end: u64,
    channel: u16,
    object_id: &[u8],
    blocks_span: Span,
    buses: &mut Vec<Bus>,
    log: &mut Log,
) -> Result<(), Error> {
    let scene = r.scene;
    let s = &scene.snapshot;
    let blocks = span(&s.direct_blocks, blocks_span);
    let states = span(&scene.overlay.direct_blocks, blocks_span);
    let mut spatial = Bus::new(channel, SourceMode::AmbienceBed, object_id);
    let mut direct = Bus::new(channel, SourceMode::PointSource, object_id);
    direct.is_direct = true;
    let mut lfe = Bus::new(channel, SourceMode::Bypass, object_id);
    lfe.is_lfe = true;
    for b in in_effect(blocks, |b| b.start) {
        let ds = &blocks[b];
        let has_position = states[b].has_position;
        let ds_labels = labels(scene, ds.labels);
        let position_or_front = |log: &mut Log| {
            if has_position {
                [ds.azimuth, ds.elevation]
            } else {
                log(
                    MODULE_WARNING,
                    b"DirectSpeakers block has no nominal position; using front-centre \
                      (azimuth 0, elevation 0)"
                        .to_vec(),
                );
                [0., 0.]
            }
        };
        let bed_distance = if has_position {
            distance(ds.distance)
        } else {
            1.
        };
        let target = routing::lfe_target(&ds_labels, ds.low_pass_present != 0);
        let mut event = Event::silent(ds.start, min(ds.end, object_end));
        let bus = if target != LfeTarget::None {
            if has_position {
                event.azimuth = sm_azimuth(ds.azimuth);
                event.elevation = elevation(ds.elevation);
                event.distance = distance(ds.distance);
            }
            &mut lfe
        } else if routing == Routing::Matrix {
            let Some(resolved) = matrix else {
                return Err(error(
                    1,
                    "DirectSpeakers matrix routing requires a parsed matrix",
                    Vec::new(),
                ));
            };
            let route = &resolved[routing::matrix_route(resolved, &ds_labels)?];
            event.outputs = route.targets.iter().map(|t| (t.output, t.gain)).collect();
            &mut direct
        } else if routing == Routing::Label {
            match routing::target_index(targets, &ds_labels) {
                Some(t) if !targets[t].is_lfe => {
                    event.outputs.push((t, 1.));
                    &mut direct
                }
                _ => {
                    let from_label = routing::label_position(&ds_labels);
                    let [azimuth, el] = match from_label {
                        Some(p) => p,
                        None => position_or_front(log),
                    };
                    event.azimuth = sm_azimuth(azimuth);
                    event.elevation = elevation(el);
                    event.distance = bed_distance;
                    let label = ds_labels.first().copied().unwrap_or(b"<missing>");
                    log(
                        MODULE_WARNING,
                        concat(&[
                            b"DirectSpeakers label '",
                            label,
                            "' not in output layout — spatializing from ".as_bytes(),
                            if from_label.is_some() {
                                b"label direction"
                            } else {
                                b"nominal position"
                            },
                        ]),
                    );
                    &mut spatial
                }
            }
        } else {
            let [azimuth, el] = position_or_front(log);
            event.azimuth = sm_azimuth(azimuth);
            event.elevation = elevation(el);
            event.distance = bed_distance;
            &mut spatial
        };
        if bus.label_key.is_empty()
            && let Some(first) = ds_labels.first()
        {
            bus.label_key = routing::canonical_label(first);
        }
        event.gain = ds.gain * object_gain;
        event.lfe = target;
        event.head_locked = ds.head_locked != 0;
        bus.events.push(event);
    }
    for bus in [spatial, direct, lfe] {
        if !bus.events.is_empty() {
            buses.push(bus);
        }
    }
    Ok(())
}

/// `std::min` of two sample positions.
fn min(a: u64, b: u64) -> u64 {
    if b < a { b } else { a }
}
