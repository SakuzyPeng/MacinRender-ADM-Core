//! Private boundary of `mradm-render`: the shared DirectSpeakers, label and
//! LFE routing rules, the offline loudspeaker driver the caller steps block by block, and the VBAP
//! backend with its file stream. Inputs are borrowed for the call; a resolved matrix is owned by its handle
//! and its view lives until `mradm_render_matrix_destroy`. Errors fill the caller's message and
//! context buffers (capacity in, length written out, NUL-terminated) with the C++ bytes.
use crate::adm::Text;
pub(crate) use mradm_engine::scene::renderer::Renderer as LiveRenderer;
use mradm_render::routing::{self, Error, LfeTarget, MatrixRoute, Override};
use mradm_render::{live, offline, triple_balance_stream};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::Arc,
};

#[repr(C)]
pub struct RenderMatrixTarget {
    pub(crate) label: Text,
    pub(crate) gain: f32,
}

#[repr(C)]
pub struct RenderMatrixRoute {
    pub(crate) source_label: Text,
    pub(crate) mute: u32,
    pub(crate) targets: *const RenderMatrixTarget,
    pub(crate) targets_len: usize,
}

#[repr(C)]
pub struct RenderOverride {
    pub object_id: Text,
    pub speaker_label: Text,
    pub mute: u32,
    pub gain_db: f32,
    pub has_head_locked: u32,
    pub head_locked: u32,
}

/// level: 1 info, 2 warning (`adm::LogLevel`); the message is static text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLogLine {
    level: u32,
    message: Text,
}

#[repr(C)]
pub struct RenderLfeRouting {
    applies_to_22_2: u32,
    split_power: u32,
    has_lfe1: u32,
    has_lfe2: u32,
    logs: [RenderLogLine; 2],
    logs_len: usize,
}

/// A resolved DirectSpeakers matrix with the records its view points into.
pub(crate) fn bad(message: &str) -> Error {
    Error {
        code: 1,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

pub(crate) unsafe fn input<'a, T>(data: *const T, len: usize) -> Result<&'a [T], Error> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<T>())
        || len > isize::MAX as usize / size_of::<T>()
    {
        return Err(bad("无效渲染缓冲区"));
    }
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

pub(crate) unsafe fn bytes<'a>(text: &Text) -> Result<&'a [u8], Error> {
    unsafe { input(text.data, text.len) }
}

/// Copies `detail` into `out` (capacity `*len`), NUL-terminated, and stores the length written.
unsafe fn write(detail: &[u8], out: *mut u8, len: *mut usize) {
    if len.is_null() {
        return;
    }
    let capacity = unsafe { *len };
    let written = if out.is_null() || capacity == 0 {
        0
    } else {
        let n = detail.len().min(capacity - 1);
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), out, n);
            *out.add(n) = 0;
        }
        n
    };
    unsafe {
        *len = written;
    }
}

pub(crate) fn boundary(
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
    f: impl FnOnce() -> Result<(), Error>,
) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(_) => Some(Error {
            code: 6,
            message: b"Rust render panic".to_vec(),
            context: Vec::new(),
        }),
    };
    let (detail, location) = error
        .as_ref()
        .map_or((&[][..], &[][..]), |e| (&e.message[..], &e.context[..]));
    unsafe {
        write(detail, message, message_len);
        write(location, context, context_len);
    }
    error.map_or(0, |e| e.code)
}

unsafe fn override_value<'a>(o: &RenderOverride) -> Result<Override<'a>, Error> {
    Ok(Override {
        object_id: unsafe { bytes(&o.object_id)? },
        speaker_label: unsafe { bytes(&o.speaker_label)? },
        mute: o.mute != 0,
        gain_db: o.gain_db,
        head_locked: (o.has_head_locked != 0).then_some(o.head_locked != 0),
    })
}

/// The live overrides of one input channel; `key` is its canonical speaker label (empty for
/// Objects and HOA channels). Bit 1: a gain multiplier was written; bit 2: an explicit head lock
/// (0 or 1) was written; otherwise the channel inherits. Null output pointers skip that field.
/// Valid queries borrow the records and do not allocate, even for noncanonical labels.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_override(
    values: *const RenderOverride,
    values_len: usize,
    object_id: Text,
    key: Text,
    gain: *mut f32,
    head_locked: *mut u32,
) -> u32 {
    catch_unwind(|| {
        let (Ok(o), Ok(id), Ok(key)) = (
            unsafe { input(values, values_len) },
            unsafe { bytes(&object_id) },
            unsafe { bytes(&key) },
        ) else {
            return 0;
        };
        let Ok((g, h)) = routing::try_live_override(
            o.iter().map(|o| unsafe { override_value(o) }),
            id,
            key,
            !gain.is_null(),
            !head_locked.is_null(),
        ) else {
            return 0;
        };
        let mut flags = 0;
        if let (Some(g), Some(out)) = (g, unsafe { gain.as_mut() }) {
            *out = g;
            flags |= 1;
        }
        if let (Some(h), Some(out)) = (h, unsafe { head_locked.as_mut() }) {
            *out = u32::from(h);
            flags |= 2;
        }
        flags
    })
    .unwrap_or(0)
}

/// The gain from one LFE input target to one 22.2 LFE output (0, 1, 2) under `routing`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_lfe_gain(
    routing: *const RenderLfeRouting,
    input_target: u32,
    output_target: u32,
) -> f32 {
    let Some(r) = (unsafe { routing.as_ref() }) else {
        return 0.;
    };
    let target = |t| match t {
        1 => LfeTarget::Lfe1,
        2 => LfeTarget::Lfe2,
        _ => LfeTarget::None,
    };
    routing::LfeRouting {
        applies_to_22_2: r.applies_to_22_2 != 0,
        split_power: r.split_power != 0,
        has_lfe1: r.has_lfe1 != 0,
        has_lfe2: r.has_lfe2 != 0,
    }
    .gain(target(input_target), target(output_target))
}

/// Like `boundary` for the driver: the locator kind (0 none, 1 `path=<input>`, 2 `input=<input>`,
/// 3 `meter`, 4 `HOA DSP`, 5 `Triple Balance DSP`, 6 `room-binaural`) and the message, cut at a
/// character boundary as the WAVE reader did.
fn driver_boundary(
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
    f: impl FnOnce() -> offline::Result<()>,
) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(_) => Some(offline::Error {
            code: 6,
            message: "Rust render panic".to_owned(),
            location: offline::Location::None,
        }),
    };
    if let Some(out) = unsafe { location.as_mut() } {
        *out = match error.as_ref().map(|e| e.location) {
            None | Some(offline::Location::None) => 0,
            Some(offline::Location::Path) => 1,
            Some(offline::Location::Input) => 2,
            Some(offline::Location::Meter) => 3,
            Some(offline::Location::HoaDsp) => 4,
            Some(offline::Location::TripleBalanceDsp) => 5,
            Some(offline::Location::Room) => 6,
        };
    }
    let detail = error.as_ref().map_or("", |e| e.message.as_str());
    if let Some(capacity) = unsafe { message_len.as_ref() }.copied() {
        let mut n = detail.len().min(capacity.saturating_sub(1));
        while !detail.is_char_boundary(n) {
            n -= 1;
        }
        unsafe { write(&detail.as_bytes()[..n], message, message_len) };
    }
    error.map_or(0, |e| e.code)
}

fn driver_error(message: &str) -> offline::Error {
    offline::Error {
        code: 1,
        message: message.to_owned(),
        location: offline::Location::None,
    }
}

/// A prepared backend (`mradm-engine::backend`) or Apple island, or the log lines, error and
/// Triple Balance report of a failed preparation.
pub struct RenderPrepared {
    pub(crate) logs: Vec<mradm_engine::request::Log>,
    pub(crate) log_views: Vec<crate::engine::EngineLog>,
    pub(crate) error: Option<mradm_engine::request::Error>,
    pub(crate) report: Option<String>,
    pub(crate) prepared: Option<Arc<mradm_engine::backend::Prepared>>,
    /// The Apple island's preparation (a render's cache only).
    pub(crate) apple: Option<Arc<apple_island::Preparation>>,
}

impl RenderPrepared {
    pub(crate) fn plan(&self) -> Option<&mradm_engine::backend::Prepared> {
        self.prepared.as_deref()
    }
}

pub(crate) fn room_polar(
    azimuth: f32,
    elevation: f32,
    distance: f32,
) -> mradm_dsp::Result<[f32; 3]> {
    // The former C++ preparation used mradm_room_polar: only this conversion enters the
    // ROOM environment. Preserve that scope for the Rust event table's gain/offset arithmetic.
    let _environment = crate::room_environment::Environment::enter();
    mradm_dsp::room::loading::polar(azimuth, elevation, distance)
}

/// A borrowed DirectSpeakers matrix.
pub(crate) unsafe fn matrix_routes<'a>(
    data: *const RenderMatrixRoute,
    len: usize,
) -> Result<Vec<MatrixRoute<'a>>, Error> {
    unsafe { input(data, len)? }
        .iter()
        .map(|m| {
            Ok(MatrixRoute {
                source_label: unsafe { bytes(&m.source_label)? },
                mute: m.mute != 0,
                targets: unsafe { input(m.targets, m.targets_len)? }
                    .iter()
                    .map(|t| Ok((unsafe { bytes(&t.label)? }, t.gain)))
                    .collect::<Result<_, Error>>()?,
            })
        })
        .collect()
}

/// A live edit (`LiveObjectOverride`): the common override and the topology scales.
#[repr(C)]
pub struct RenderLiveOverride {
    pub base: RenderOverride,
    pub extent_scale: f32,
    pub extent_width_scale: f32,
    pub extent_height_scale: f32,
    pub extent_depth_scale: f32,
    pub diffuse_scale: f32,
    pub divergence_scale: f32,
}

unsafe fn live_overrides<'a>(
    values: *const RenderLiveOverride,
    values_len: usize,
) -> offline::Result<Vec<triple_balance_stream::LiveOverride<'a>>> {
    unsafe { input(values, values_len) }
        .and_then(|values| {
            values
                .iter()
                .map(|o| {
                    Ok(triple_balance_stream::LiveOverride {
                        base: unsafe { override_value(&o.base)? },
                        extent_scale: o.extent_scale,
                        extent_width_scale: o.extent_width_scale,
                        extent_height_scale: o.extent_height_scale,
                        extent_depth_scale: o.extent_depth_scale,
                        diffuse_scale: o.diffuse_scale,
                        divergence_scale: o.divergence_scale,
                    })
                })
                .collect::<Result<Vec<_>, Error>>()
        })
        .map_err(|e| driver_error(&String::from_utf8_lossy(&e.message)))
}

unsafe fn checked_output<'a>(data: *mut f32, len: usize) -> Option<&'a mut [f32]> {
    if len == 0 {
        return Some(&mut []);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<f32>())
        || len > isize::MAX as usize / size_of::<f32>()
    {
        return None;
    }
    Some(unsafe { slice::from_raw_parts_mut(data, len) })
}

/// `live_scene::ElementDescriptor`; role 0 object, 1 DirectSpeakers, 2 LFE. `object_id` is the
/// semantic identity's AudioObject id, empty without one.
#[repr(C)]
pub struct RenderLiveElement {
    element_id: u64,
    role: u32,
    speaker_label: Text,
    object_id: Text,
    has_position: u32,
    position: [f32; 3],
}

/// `live_scene::ObjectState`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLiveState {
    pub valid_fields: u64,
    pub active: u32,
    pub linear_gain: f32,
    pub position: [f32; 3],
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    pub diffuse: f32,
    pub divergence: f32,
    pub channel_lock: u32,
    pub screen_reference: u32,
    pub head_locked: u32,
    pub divergence_azimuth_range: f32,
    pub divergence_position_range: f32,
    pub has_max_distance: u32,
    pub max_distance: f32,
}

/// A PCM plane; the samples are read only with a signal.
#[repr(C)]
pub struct RenderLivePlane {
    element_id: u64,
    has_signal: u32,
    samples: *const f32,
    samples_len: usize,
}

/// A call-scoped projection of already validated transport records.
struct LivePlanes<'a> {
    records: &'a [RenderLivePlane],
}

impl<'a> LivePlanes<'a> {
    /// All signalled sample ranges must be valid for `'a` and disjoint from mutable output/state.
    unsafe fn new(records: &'a [RenderLivePlane]) -> Self {
        Self { records }
    }
}

impl live::PlaneSource for LivePlanes<'_> {
    fn plane(&self, index: usize) -> live::Plane<'_> {
        let p = &self.records[index];
        live::Plane {
            element_id: p.element_id,
            has_signal: p.has_signal != 0,
            samples: if p.has_signal != 0 && p.samples_len != 0 {
                // The constructor's ranges remain borrowed only for this render call.
                unsafe { slice::from_raw_parts(p.samples, p.samples_len) }
            } else {
                &[]
            },
        }
    }

    fn iter(&self) -> impl Iterator<Item = live::Plane<'_>> + Clone {
        (0..self.records.len()).map(|i| self.plane(i))
    }
}

#[repr(C)]
pub struct RenderLiveEntry {
    pub element_id: u64,
    pub state: RenderLiveState,
}

#[repr(C)]
pub struct RenderLiveUpdate {
    pub element_id: u64,
    pub offset: u32,
    pub ramp: u32,
    pub jump: u32,
    pub changed: u64,
    pub cleared: u64,
    pub state: RenderLiveState,
}

/// One `live_scene::Frame`; `trace` is the checkpoint prefix, empty for none.
#[repr(C)]
pub struct RenderLiveFrame {
    generation: u64,
    duration: u32,
    planes: *const RenderLivePlane,
    planes_len: usize,
    initial: *const RenderLiveEntry,
    initial_len: usize,
    updates: *const RenderLiveUpdate,
    updates_len: usize,
    trace: Text,
}

/// A warning of the rendered frame (`live_scene::DiagnosticCode`); the message is static text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLiveDiagnostic {
    code: u32,
    element_id: u64,
    field: u64,
    message: Text,
}

/// A Scene stream renderer (`live_scene::ILiveSceneRenderer`): `mradm-engine`'s renderer, with the
/// record buffers of this boundary.
pub struct RenderLive {
    pub(crate) renderer: LiveRenderer,
    /// The checkpoint name suffix (`mradm_engine_scene_renderer_create`'s view points at it).
    pub(crate) suffix: String,
    initial: Vec<live::Entry>,
    updates: Vec<live::Update>,
    diagnostics: Vec<RenderLiveDiagnostic>,
}

impl RenderLive {
    /// Publishes the renderer's warnings of the last frame (or, `configured`, configure) as the
    /// handle's diagnostic records.
    fn publish(
        &mut self,
        configured: bool,
        list: &mut *const RenderLiveDiagnostic,
        count: &mut usize,
    ) {
        let warnings = if configured {
            self.renderer.configured()
        } else {
            self.renderer.diagnostics()
        };
        self.diagnostics.clear();
        self.diagnostics
            .extend(warnings.iter().map(|d| RenderLiveDiagnostic {
                code: d.code,
                element_id: d.element_id,
                field: d.field,
                message: Text {
                    data: d.message.as_ptr(),
                    len: d.message.len(),
                },
            }));
        *list = self.diagnostics.as_ptr();
        *count = self.diagnostics.len();
    }

    pub(crate) fn new(renderer: LiveRenderer) -> Self {
        Self {
            suffix: renderer.trace_suffix(),
            renderer,
            initial: Vec::new(),
            updates: Vec::new(),
            diagnostics: Vec::new(),
        }
    }
}

fn live_state(s: &RenderLiveState) -> live::State {
    live::State {
        valid_fields: s.valid_fields,
        active: s.active != 0,
        linear_gain: s.linear_gain,
        position: s.position,
        width: s.width,
        height: s.height,
        depth: s.depth,
        diffuse: s.diffuse,
        divergence: s.divergence,
        channel_lock: s.channel_lock != 0,
        screen_reference: s.screen_reference != 0,
        head_locked: s.head_locked != 0,
        divergence_azimuth_range: s.divergence_azimuth_range,
        divergence_position_range: s.divergence_position_range,
        channel_lock_max_distance: (s.has_max_distance != 0).then_some(s.max_distance),
    }
}

unsafe fn live_renderer<'a>(handle: *mut RenderLive) -> Result<&'a mut RenderLive, Error> {
    unsafe { handle.as_mut() }.ok_or_else(|| bad("无效 Scene 流式渲染句柄"))
}

/// `live_scene::RendererConfig`. `selection` is the `RendererSelection`
/// value; `speaker_spread` / `binaural_spread` the C++ enum values; `has_sofa` marks a non-empty
/// SOFA path (UTF-8); `reference` holds the default geometry when absent; `room_profile` may be
/// null; `sink` is whether the caller publishes diagnostics.
#[repr(C)]
pub struct EngineSceneRendererRequest {
    pub selection: u32,
    pub layout_id: Text,
    pub apple_geometry: u32,
    pub speaker_spread: u32,
    pub binaural_spread: u32,
    pub split_power: u32,
    pub smoothing_frames: u32,
    pub sample_rate: u32,
    pub has_sofa: u32,
    pub sofa_path: Text,
    pub sofa_file_name: Text,
    pub has_reference: u32,
    pub reference: mradm_dsp::room::screen::RoomScreenGeometry,
    pub has_reproduction: u32,
    pub reproduction: mradm_dsp::room::screen::RoomScreenGeometry,
    pub room_profile: *const crate::room::RoomProfileHandle,
    pub room_hrir: Text,
    pub room_metres: f64,
    pub room_quality: u32,
    pub sink: u32,
}

/// The renderer configuration of a request; also the Scene stream's (`engine_scene`).
unsafe fn scene_renderer_config(
    r: &EngineSceneRendererRequest,
) -> Result<mradm_engine::scene::renderer::Config<'_>, Error> {
    use crate::engine_render::{binaural_spread_of, speaker_spread_of};
    use mradm_engine::backend::{RoomRequest, Screens, Sofa};
    let (Some(selection), Some(speaker_spread), Some(binaural_spread)) = (
        mradm_engine::request::Selection::from_u32(r.selection),
        speaker_spread_of(r.speaker_spread),
        binaural_spread_of(r.binaural_spread),
    ) else {
        return Err(bad("无效 Scene 流式渲染参数"));
    };
    Ok(mradm_engine::scene::renderer::Config {
        selection,
        layout_id: unsafe { bytes(&r.layout_id)? },
        apple_geometry: r.apple_geometry != 0,
        speaker_spread,
        binaural_spread,
        split_power: r.split_power != 0,
        smoothing_frames: r.smoothing_frames,
        sample_rate: r.sample_rate,
        sofa: if r.has_sofa != 0 {
            Some(Sofa {
                path: unsafe { bytes(&r.sofa_path)? },
                file_name: unsafe { bytes(&r.sofa_file_name)? },
            })
        } else {
            None
        },
        screens: Screens {
            has_reference: r.has_reference != 0,
            reference: r.reference,
            has_reproduction: r.has_reproduction != 0,
            reproduction: r.reproduction,
        },
        room: RoomRequest {
            package: unsafe { crate::room::package(r.room_profile) },
            hrir: unsafe { bytes(&r.room_hrir)? },
            metres: r.room_metres,
            quality: r.room_quality,
        },
        sink: r.sink != 0,
    })
}

/// Creates the Scene stream renderer of the configuration (`mradm-engine` `scene::renderer`):
/// `*sample_rate` is the rate it renders at, `*suffix` its checkpoint name suffix (valid while the
/// handle lives).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_renderer_create(
    request: *const EngineSceneRendererRequest,
    output: *mut *mut RenderLive,
    sample_rate: *mut u32,
    suffix: *mut Text,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let (Some(out), Some(rate), Some(suffix), Some(r)) = (
            unsafe { output.as_mut() },
            unsafe { sample_rate.as_mut() },
            unsafe { suffix.as_mut() },
            unsafe { request.as_ref() },
        ) else {
            return Err(bad("无效 Scene 流式渲染参数"));
        };
        *out = ptr::null_mut();
        let config = unsafe { scene_renderer_config(r)? };
        let created =
            mradm_engine::scene::renderer::create(&config, &crate::engine_render::FfiHost)?;
        let handle = Box::new(RenderLive::new(created.renderer));
        *rate = created.sample_rate;
        *suffix = Text {
            data: handle.suffix.as_ptr(),
            len: handle.suffix.len(),
        };
        *out = Box::into_raw(handle);
        Ok(())
    })
}

#[path = "engine_scene.rs"]
pub mod engine_scene;
#[path = "engine_scene_output.rs"]
pub mod engine_scene_output;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_destroy(handle: *mut RenderLive) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The output channels; 0 for a null handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_channels(handle: *const RenderLive) -> u32 {
    unsafe { handle.as_ref() }.map_or(0, |h| h.renderer.output_channels())
}

/// Replaces the element generation; a failure keeps the previous one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_configure(
    handle: *mut RenderLive,
    generation: u64,
    elements: *const RenderLiveElement,
    elements_len: usize,
    diagnostics: *mut *const RenderLiveDiagnostic,
    diagnostics_len: *mut usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { live_renderer(handle)? };
        let elements = unsafe { input(elements, elements_len)? }
            .iter()
            .map(|e| {
                Ok(live::Element {
                    id: e.element_id,
                    role: match e.role {
                        0 => Some(live::Role::Object),
                        1 => Some(live::Role::DirectSpeaker),
                        2 => Some(live::Role::Lfe),
                        _ => None,
                    },
                    speaker_label: unsafe { bytes(&e.speaker_label)? },
                    object_id: unsafe { bytes(&e.object_id)? },
                    has_position: e.has_position != 0,
                    position: e.position,
                })
            })
            .collect::<Result<Vec<_>, Error>>()?;
        let result = h.renderer.configure(generation, &elements);
        if let (Some(list), Some(count)) = (unsafe { diagnostics.as_mut() }, unsafe {
            diagnostics_len.as_mut()
        }) {
            h.publish(true, list, count);
        }
        result
    })
}

/// The listener orientation of the next render (room latches a rejected one; binaural rotates).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_pose(
    handle: *mut RenderLive,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Some(h) = unsafe { handle.as_mut() } {
        h.renderer.pose([yaw, pitch, roll]);
    }
}

/// Ends the input; `*tail` is the frames the renderer appends (0 without a room).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_end_input(
    handle: *mut RenderLive,
    tail: *mut u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let (Some(h), Some(out)) = (unsafe { handle.as_mut() }, unsafe { tail.as_mut() }) else {
            return Err(bad("无效 Scene 流式渲染句柄"));
        };
        *out = h.renderer.end_input()?;
        Ok(())
    })
}

/// The input frames the renderer still appends after the end (room tail, binaural convolution).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_tail(handle: *const RenderLive) -> u64 {
    unsafe { handle.as_ref() }.map_or(0, |h| h.renderer.tail())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_reset(handle: *mut RenderLive) {
    if let Some(h) = unsafe { handle.as_mut() } {
        h.renderer.reset();
    }
}

/// Renders the frame into the start of `output`. `*diagnostics` lists the frame's warnings until
/// the next call on the handle, also after a failure: room publishes those raised before it, VBAP
/// and Triple Balance none, and leave the output unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_render(
    handle: *mut RenderLive,
    frame: *const RenderLiveFrame,
    output: *mut f32,
    output_len: usize,
    diagnostics: *mut *const RenderLiveDiagnostic,
    diagnostics_len: *mut usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        if handle.is_null() {
            return Err(bad("无效 Scene 流式渲染句柄"));
        }
        // The handle is not borrowed yet: only its renderer kind is read.
        let (name, dsp) = unsafe { (*handle).renderer.names() };
        let (Some(list), Some(count)) = (unsafe { diagnostics.as_mut() }, unsafe {
            diagnostics_len.as_mut()
        }) else {
            return Err(bad(&format!("无效 {name} 参数")));
        };
        *list = ptr::null();
        *count = 0;
        // Retain the mixer boundary's rejection of overlapping PCM. Validate the raw
        // ranges before borrowing the handle, samples or mutable output.
        let invalid_buffers = |_| Error {
            code: 1,
            message: format!("Invalid {name} FFI arguments").into_bytes(),
            context: dsp.to_vec(),
        };
        if frame.is_null() {
            return Err(bad(&format!("无效 {name} 参数")));
        }
        crate::checked::separate(handle, 1, frame, 1).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, output, output_len).map_err(invalid_buffers)?;
        crate::checked::separate(frame, 1, output, output_len).map_err(invalid_buffers)?;
        let f = unsafe { frame.as_ref() }.ok_or_else(|| bad(&format!("无效 {name} 参数")))?;
        crate::checked::separate(handle, 1, f.planes, f.planes_len).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, f.initial, f.initial_len).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, f.updates, f.updates_len).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, f.trace.data, f.trace.len).map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.planes, f.planes_len)
            .map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.initial, f.initial_len)
            .map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.updates, f.updates_len)
            .map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.trace.data, f.trace.len)
            .map_err(invalid_buffers)?;
        let records = unsafe { input(f.planes, f.planes_len)? };
        for p in records.iter().filter(|p| p.has_signal != 0) {
            crate::checked::separate(handle, 1, p.samples, p.samples_len)
                .map_err(invalid_buffers)?;
            crate::checked::separate(output, output_len, p.samples, p.samples_len)
                .map_err(invalid_buffers)?;
        }
        // Raw sample ranges and overlap were checked above; no view outlives this call.
        let planes = unsafe { LivePlanes::new(records) };
        let h = unsafe { live_renderer(handle)? };
        h.initial.clear();
        h.initial.extend(
            unsafe { input(f.initial, f.initial_len)? }
                .iter()
                .map(|e| live::Entry {
                    element_id: e.element_id,
                    state: live_state(&e.state),
                }),
        );
        h.updates.clear();
        h.updates
            .extend(
                unsafe { input(f.updates, f.updates_len)? }
                    .iter()
                    .map(|u| live::Update {
                        element_id: u.element_id,
                        offset: u.offset,
                        ramp: u.ramp,
                        jump: u.jump != 0,
                        changed: u.changed,
                        cleared: u.cleared,
                        state: live_state(&u.state),
                    }),
            );
        let output = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| bad(&format!("无效 {name} 输出")))?;
        let result = h.renderer.render(
            &live::FrameView {
                generation: f.generation,
                duration: f.duration,
                planes: &planes,
                initial: &h.initial,
                updates: &h.updates,
                trace: unsafe { bytes(&f.trace)? },
            },
            output,
        );
        // A room renderer publishes its warnings as they arise, also before a failure.
        h.publish(false, list, count);
        result
    })
}

#[path = "render_room.rs"]
pub mod room_ffi;

// The binaural file graph's allocation test (the allocation counter lives in this crate).
#[cfg(test)]
#[path = "render_binaural_test.rs"]
mod binaural_allocations;

#[path = "render_stream.rs"]
mod stream_ffi;

#[path = "apple_island.rs"]
pub mod apple_island;
#[path = "device_island.rs"]
pub mod device_island;
#[path = "engine_monitor.rs"]
pub mod engine_monitor;
#[path = "engine_monitor_session.rs"]
pub mod engine_monitor_session;

#[cfg(test)]
#[path = "../../mradm-render/tests/common/mod.rs"]
mod test_common;

#[cfg(test)]
#[path = "render_stream_test.rs"]
mod stream_tests;

#[cfg(test)]
#[path = "render_live_test.rs"]
mod live_tests;

#[cfg(all(test, any(target_arch = "x86_64", target_arch = "aarch64")))]
#[path = "render_room_test.rs"]
mod room_tests;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;

    fn record(id: &[u8], label: &[u8], gain_db: f32, head: Option<bool>) -> RenderOverride {
        RenderOverride {
            object_id: Text::bytes(id),
            speaker_label: Text::bytes(label),
            mute: 0,
            gain_db,
            has_head_locked: u32::from(head.is_some()),
            head_locked: u32::from(head.unwrap_or(false)),
        }
    }

    #[test]
    fn borrowed_live_queries_do_not_allocate_and_select_fields_independently() {
        let long_label = b"_x".repeat(256);
        let long_key = b"X".repeat(256);
        let mut records: Vec<_> = (0..128)
            .map(|_| record(b"other", b"m_+030", -6., Some(false)))
            .collect();
        records.extend([
            record(b"bed", b"", -12., Some(false)),
            record(b"bed", b"", 0., Some(true)),
            record(b"bed", b"M+030", -12., Some(false)),
            record(b"bed", b"m_+030 \xc3\xa9", -6., Some(true)),
            record(b"bed", b"M+030", -6., None),
            record(b"bed", b"M-030", 0., Some(false)),
            record(b"bed", &long_label, 0., Some(false)),
        ]);
        // A gain-only channel mute must not shadow the last explicit head-lock value.
        records[132].mute = 1;
        let cases: &[(&[u8], f32, u32)] = &[
            (b"M+030", 0., 1),
            (b"M-030", 1., 0),
            (b"", 1., 1),
            (b"L", 1., 1), // live labels are canonicalized, not alias-resolved
            (&long_key, 1., 0),
        ];
        let mut correct = true;
        COUNT.with(|c| c.set(Some(0)));
        for _ in 0..100 {
            for &(key, expected_gain, expected_head) in cases {
                for fields in 0..4 {
                    let (mut gain, mut head) = (17., 19);
                    let flags = unsafe {
                        mradm_render_live_override(
                            records.as_ptr(),
                            records.len(),
                            Text::bytes(b"bed"),
                            Text::bytes(key),
                            if fields & 1 != 0 {
                                &mut gain
                            } else {
                                ptr::null_mut()
                            },
                            if fields & 2 != 0 {
                                &mut head
                            } else {
                                ptr::null_mut()
                            },
                        )
                    };
                    correct &= flags == fields
                        && gain == if fields & 1 != 0 { expected_gain } else { 17. }
                        && head == if fields & 2 != 0 { expected_head } else { 19 };
                }
            }
        }
        let allocations = COUNT.with(|c| c.replace(None).unwrap());
        assert!(correct, "live precedence and nullable outputs");
        assert_eq!(
            allocations, 0,
            "including the first lookup and label folding"
        );
    }

    #[test]
    fn live_queries_preserve_outputs_when_a_later_record_is_invalid() {
        let mut records = [
            record(b"bed", b"", 0., Some(true)),
            record(b"other", b"", 0., None),
        ];
        records[1].speaker_label = Text {
            data: ptr::null(),
            len: 1,
        };
        let (mut gain, mut head) = (17., 19);
        let flags = unsafe {
            mradm_render_live_override(
                records.as_ptr(),
                records.len(),
                Text::bytes(b"bed"),
                Text::bytes(b"M+030"),
                &mut gain,
                &mut head,
            )
        };
        assert_eq!((flags, gain, head), (0, 17., 19));
        records[1] = record(b"other", b"", 0., None);
        let flags = unsafe {
            mradm_render_live_override(
                records.as_ptr(),
                records.len(),
                Text::bytes(b"missing"),
                Text::bytes(b"M+030"),
                &mut gain,
                &mut head,
            )
        };
        assert_eq!((flags, gain, head), (0, 17., 19));
    }
}
