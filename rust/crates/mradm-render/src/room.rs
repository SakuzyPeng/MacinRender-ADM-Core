//! The room file renderer's sources and event table (PCM consistency 4.6), moved from the C++
//! `RoomRenderer::prepare` with the same checks, order, arithmetic and texts: one source per
//! track with blocks, the in-effect Objects and DirectSpeakers blocks as activation, state and
//! deactivation events, the reference screen of screen-referenced objects, and the stable
//! `(sample, priority, element)` order. Object ramps keep the C++ `default_interp_ms × 48`
//! (half length at 96 kHz; a known issue, PCM consistency 4.6).
use crate::routing::{self, Error, Level};
use crate::scene::{self, Log, in_effect, labels, span};
use mradm_adm::records::{AdmPosition, AdmScreen, Object};
use mradm_dsp::room::control::{RoomCommand, RoomDescription, State};
use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
use mradm_scene::{RenderScene, SourceKind};

/// Every live state field (`(1 << 11) - 1`).
const FIELDS: u64 = (1 << 11) - 1;
const MAX_SOURCES: usize = 512;
const SCREEN_WARNING: &str = "未配置重放屏幕，screenRef 保留原方向";
const FRONT_WARNING: &str =
    "DirectSpeakers block has no nominal position; using front-centre (azimuth 0, elevation 0)";
pub const PREPARED_INFO: &str = "已准备连续位置房间渲染；FIFO 为 256 帧，完整节目追加有限尾声";

/// `BinauralSpreadMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Spread {
    Automatic,
    None,
    SafSpreader,
}

/// `DirectSpeakersRoutingMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Routing {
    Automatic,
    Label,
    Position,
    Matrix,
}

/// The screen configuration (`ScreenConfig`), already validated by the caller.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Screens {
    pub reference: Option<RoomScreenGeometry>,
    pub reproduction: Option<RoomScreenGeometry>,
}

pub struct Request<'a> {
    pub scene: &'a RenderScene,
    /// The prepared room's sample rate.
    pub sample_rate: u32,
    pub layout_id: &'a [u8],
    pub spread: Spread,
    pub routing: Routing,
    pub has_matrix: bool,
    pub default_interp_ms: u32,
    pub screens: Screens,
}

/// One command at an input sample; `priority` orders commands sharing it.
#[derive(Clone, Copy)]
pub struct Event {
    pub sample: u64,
    pub priority: u32,
    pub command: RoomCommand,
}

/// One source per rendered track.
pub struct Prepared {
    pub descriptions: Vec<RoomDescription>,
    pub channels: Vec<u16>,
    pub objects: Vec<Vec<u8>>,
    /// The canonical label of the first in-effect DirectSpeakers block, empty without one.
    pub labels: Vec<Vec<u8>>,
    pub events: Vec<Event>,
    pub frames: u64,
    pub input_channels: u16,
    pub spread: bool,
}

fn error(code: i32, message: &str) -> Error {
    Error {
        code,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

fn unsupported(message: &str) -> Error {
    error(2, message)
}

/// The caller supplies the polar conversion's floating-point environment boundary. Register
/// access stays in the private FFI, while gain and offset arithmetic keeps the caller's environment.
pub type PolarConversion = fn(f32, f32, f32) -> mradm_dsp::Result<[f32; 3]>;

/// The offset block position in the room's Cartesian coordinates.
fn position(
    point: AdmPosition,
    object: &Object,
    polar: PolarConversion,
) -> Result<[f32; 3], Error> {
    let p = scene::offset(point, object);
    if p.cartesian != 0 {
        Ok([p.x, p.y, p.z])
    } else {
        // Keep room_internal::polar's error text and context.
        polar(p.azimuth, p.elevation, p.distance).map_err(|e| Error {
            code: crate::offline::dsp_code(&e),
            message: "房间 DSP 操作失败".as_bytes().to_vec(),
            context: b"room-binaural".to_vec(),
        })
    }
}

fn geometry(screen: &AdmScreen) -> RoomScreenGeometry {
    RoomScreenGeometry {
        cartesian: screen.cartesian,
        reserved: 0,
        aspect_ratio: screen.aspect_ratio,
        centre: screen.centre,
        width: screen.width,
    }
}

fn same_screen(a: &RoomScreenGeometry, b: &RoomScreenGeometry) -> bool {
    a.cartesian == b.cartesian
        && a.aspect_ratio == b.aspect_ratio
        && a.centre == b.centre
        && a.width == b.width
}

/// Whether `wanted` is among `pending` or, with ADM provenance, their nested objects.
fn contains_object<'a>(scene: &'a RenderScene, mut pending: Vec<&'a [u8]>, wanted: &[u8]) -> bool {
    let s = &scene.snapshot;
    let provenance = scene.info.source_kind == SourceKind::Adm;
    let mut visited = std::collections::BTreeSet::new();
    while let Some(id) = pending.pop() {
        if id == wanted {
            return true;
        }
        if !visited.insert(id) {
            continue;
        }
        let Some(object) = s
            .objects
            .iter()
            .find(|o| s.strings[o.id as usize].as_bytes() == id)
        else {
            continue;
        };
        if provenance {
            pending.extend(
                span(&s.indices, object.child_objects)
                    .iter()
                    .map(|&i| s.strings[i as usize].as_bytes()),
            );
        }
    }
    false
}

/// `resolve_reference_screen`: the configured reference, else (with a reproduction screen) the
/// one production screen of every programme holding the object, else the default.
pub fn reference_screen(
    scene: &RenderScene,
    object_id: &[u8],
    screens: &Screens,
) -> Result<RoomScreenGeometry, Error> {
    if let Some(reference) = screens.reference {
        return Ok(reference);
    }
    if screens.reproduction.is_none() {
        return Ok(RoomScreenGeometry::default());
    }
    let s = &scene.snapshot;
    let text = |i: u32| s.strings[i as usize].as_bytes();
    let mut selected: Option<RoomScreenGeometry> = None;
    for programme in &s.programmes {
        let member = span(&s.indices, programme.content_ids).iter().any(|&id| {
            s.contents
                .iter()
                .find(|c| text(c.id) == text(id))
                .is_some_and(|content| {
                    let objects = span(&s.indices, content.object_ids)
                        .iter()
                        .map(|&i| text(i))
                        .collect();
                    contains_object(scene, objects, object_id)
                })
        });
        if !member {
            continue;
        }
        let screen = if programme.reference_screen != 0 {
            geometry(&programme.screen_geometry)
        } else {
            RoomScreenGeometry::default()
        };
        if selected.is_some_and(|previous| !same_screen(&previous, &screen)) {
            return Err(error(
                1,
                "对象属于制作屏幕不同的 Programme；请在屏幕配置中显式覆盖 reference",
            ));
        }
        selected = Some(screen);
    }
    Ok(selected.unwrap_or_default())
}

/// `room_internal::screens`: the reproduction mapping is enabled with a reproduction screen.
pub fn room_screens(screens: &Screens, reference: &RoomScreenGeometry) -> RoomScreens {
    RoomScreens {
        enabled: u32::from(screens.reproduction.is_some()),
        reserved: 0,
        reference: *reference,
        reproduction: screens.reproduction.unwrap_or(*reference),
    }
}

/// Activation (priority 1) never waits for a positional ramp; the state (2) ramps; the
/// deactivation (0) yields to an adjacent block's activation at the same sample.
fn add_block(p: &mut Prepared, source: u32, start: u64, end: u64, duration: u64, state: &State) {
    if end <= start || start >= p.frames {
        return;
    }
    let mut command = RoomCommand {
        element: source,
        changed: 1,
        state: *state,
        ..RoomCommand::default()
    };
    p.events.push(Event {
        sample: start,
        priority: 1,
        command,
    });
    command.changed = FIELDS & !1;
    command.duration = duration.min(end - start).min(u64::from(u32::MAX)) as u32;
    p.events.push(Event {
        sample: start,
        priority: 2,
        command,
    });
    command.duration = 0;
    command.changed = 1;
    command.state.active = 0;
    p.events.push(Event {
        sample: end,
        priority: 0,
        command,
    });
}

/// Prepares the sources and events; `log` receives the `room-binaural` lines in order.
pub fn prepare(r: &Request, log: &mut Log, polar: PolarConversion) -> Result<Prepared, Error> {
    let scene = r.scene;
    let s = &scene.snapshot;
    if scene.info.sample_rate != r.sample_rate || r.layout_id != b"binaural" || !s.hoa.is_empty() {
        return Err(unsupported(
            "room-binaural 需要同采样率双耳 Objects/DirectSpeakers",
        ));
    }
    if r.spread == Spread::SafSpreader
        || r.has_matrix
        || !matches!(r.routing, Routing::Automatic | Routing::Position)
    {
        return Err(unsupported(
            "room-binaural 使用连续位置渲染，不支持此 spreader/扬声器路由模式",
        ));
    }
    let provenance = scene.info.source_kind == SourceKind::Adm;
    let mut p = Prepared {
        descriptions: Vec::new(),
        channels: Vec::new(),
        objects: Vec::new(),
        labels: Vec::new(),
        events: Vec::new(),
        frames: scene.info.frames,
        input_channels: scene.info.channels,
        spread: r.spread != Spread::None,
    };
    let mut screen_warned = false;
    for (oi, object) in s.objects.iter().enumerate() {
        let object_id = s.strings[object.id as usize].as_bytes();
        let mute = scene.overlay.objects[oi].mute;
        let tracks = span(&s.tracks, object.tracks);
        let screen_ref = tracks.iter().any(|t| {
            let blocks = span(&s.object_blocks, t.blocks);
            in_effect(blocks, |b| b.start)
                .into_iter()
                .any(|b| blocks[b].screen_ref != 0)
        });
        let reference = if screen_ref {
            reference_screen(scene, object_id, &r.screens)?
        } else {
            RoomScreenGeometry::default()
        };
        for track in tracks {
            let blocks = span(&s.object_blocks, track.blocks);
            let direct = span(&s.direct_blocks, track.ds_blocks);
            if blocks.is_empty() && direct.is_empty() {
                continue;
            }
            if track.channel < 0
                || track.channel >= i32::from(p.input_channels)
                || p.descriptions.len() >= MAX_SOURCES
                || (!blocks.is_empty() && !direct.is_empty())
            {
                return Err(unsupported("room-binaural 声源轨道无效或超过 512 路"));
            }
            let source = p.descriptions.len() as u32;
            // Only the last block at each start is in effect (S08): an overwritten block leaves
            // no role, live label, ramp origin or activity behind.
            let object_blocks = in_effect(blocks, |b| b.start);
            let direct_blocks = in_effect(direct, |b| b.start);
            let is_lfe = |b: usize| {
                routing::lfe_target(
                    &labels(scene, direct[b].labels),
                    direct[b].low_pass_present != 0,
                ) != routing::LfeTarget::None
            };
            let mut description = RoomDescription {
                id: u64::from(source) + 1,
                screens: room_screens(&r.screens, &reference),
                position: [0., 1., 0.],
                ..RoomDescription::default()
            };
            if let Some(&first) = direct_blocks.first() {
                description.role = if is_lfe(first) { 2 } else { 1 };
            }
            let role = description.role;
            p.descriptions.push(description);
            p.channels.push(track.channel as u16);
            p.objects.push(object_id.to_vec());
            p.labels.push(
                direct_blocks
                    .first()
                    .and_then(|&b| labels(scene, direct[b].labels).first().copied())
                    .map(routing::canonical_label)
                    .unwrap_or_default(),
            );
            let mut inactive = RoomCommand {
                element: source,
                changed: FIELDS,
                ..RoomCommand::default()
            };
            inactive.state.active = 0;
            p.events.push(Event {
                sample: 0,
                priority: 0,
                command: inactive,
            });
            for (i, &b) in object_blocks.iter().enumerate() {
                let block = &blocks[b];
                let point = position(block.position, object, polar)?;
                let state = State {
                    gain: if mute { 0. } else { object.gain * block.gain },
                    position: point,
                    extent: [block.width, block.height, block.depth],
                    diffuse: block.diffuse,
                    divergence: block.divergence,
                    divergence_range: [
                        block.azimuth_range,
                        if block.position.cartesian != 0 {
                            block.position_range
                        } else {
                            0.
                        },
                    ],
                    channel_lock: block.channel_lock,
                    has_max_distance: block.max_distance_present,
                    max_distance: if block.max_distance_present != 0 {
                        block.max_distance
                    } else {
                        0.
                    },
                    screen_reference: block.screen_ref,
                    head_locked: block.head_locked,
                    ..State::default()
                };
                if block.screen_ref != 0 && r.screens.reproduction.is_none() && !screen_warned {
                    log(Level::Warning, SCREEN_WARNING.as_bytes().to_vec());
                    screen_warned = true;
                }
                let next = object_blocks
                    .get(i + 1)
                    .map_or(p.frames, |&n| blocks[n].start);
                let end = block.end.min(object.end).min(next).min(p.frames);
                let ramp = if i == 0 || block.jump != 0 {
                    0
                } else if block.interpolation_present != 0 {
                    block.interpolation
                } else {
                    u64::from(r.default_interp_ms) * 48
                };
                add_block(&mut p, source, block.start, end, ramp, &state);
            }
            for (i, &b) in direct_blocks.iter().enumerate() {
                let block = &direct[b];
                let lfe = is_lfe(b);
                if (role == 2) != lfe {
                    return Err(unsupported("同一房间声源不能在全频与 LFE 角色间切换"));
                }
                let mut state = State {
                    gain: if mute { 0. } else { object.gain * block.gain },
                    head_locked: block.head_locked,
                    ..State::default()
                };
                if role != 2 {
                    let has_position =
                        scene.overlay.direct_blocks[track.ds_blocks.offset + b].has_position;
                    let mut nominal = if has_position {
                        [block.azimuth, block.elevation]
                    } else {
                        log(Level::Warning, FRONT_WARNING.as_bytes().to_vec());
                        [0., 0.]
                    };
                    if !has_position
                        && let Some(label) = routing::label_position(&labels(scene, block.labels))
                    {
                        nominal = label;
                    }
                    let mut point = AdmPosition {
                        cartesian: 0,
                        azimuth: nominal[0],
                        elevation: nominal[1],
                        distance: if has_position { block.distance } else { 1. },
                        ..AdmPosition::default()
                    };
                    let user_override =
                        scene.overlay.direct_blocks[track.ds_blocks.offset + b].position_override;
                    if provenance
                        && block.has_position != 0
                        && block.position.cartesian != 0
                        && !user_override
                    {
                        point = block.position;
                    }
                    state.position = position(point, object, polar)?;
                }
                let next = direct_blocks
                    .get(i + 1)
                    .map_or(p.frames, |&n| direct[n].start);
                let end = block.end.min(object.end).min(next).min(p.frames);
                add_block(&mut p, source, block.start, end, 0, &state);
            }
        }
    }
    p.events
        .sort_by_key(|e| (e.sample, e.priority, e.command.element));
    log(Level::Info, PREPARED_INFO.as_bytes().to_vec());
    Ok(p)
}
