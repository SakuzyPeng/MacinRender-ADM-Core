//! Scene streams and Scene outputs: `adm_scene_stream_*`, `adm_scene_output_*` and the Scene
//! stream's room exports. Each export validates the caller's records as the C++ did, converts
//! them, and calls the same `mradm-ffi` entries the C++ `SceneStreamEngine` and
//! `SceneOutputSession` shells called, with the same message buffers, so messages, locators and
//! truncation are unchanged.
use crate::context::adm_context_t;
use crate::error::{self, Error, Failure, c_str, concat, utf8_path};
use crate::guard::{self, INTERNAL, INVALID_ARGUMENT, OK, UNSUPPORTED};
use crate::options::file_name;
use crate::query::BAD_ALLOC;
use crate::room::{self, adm_room_config_t};
use crate::string;
use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{RoomRequest as EngineRoomRequest, Screens, Sofa};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer::Config as RendererConfig;
use mradm_engine::scene::stream::{
    self as engine_stream, Config as StreamConfig, Descriptor, Entity, FrameInput,
    Identity as EngineIdentity, PlaneInput, PlaneInputs, Sizes,
};
use mradm_ffi::engine_render::{binaural_spread_of, speaker_spread_of};
use mradm_ffi::hptf::settings::Settings;
use mradm_ffi::hptf::{Coefficients, Snapshot};
use mradm_ffi::render::device_island::DeviceIslands;
use mradm_ffi::render::engine_scene::{EngineSceneStream, create_stream};
use mradm_ffi::render::engine_scene_output::EngineSceneOutput;
use mradm_ffi::render::room_ffi::{RenderRoomControl, switch_config};
use mradm_render::live;
use mradm_render::room_runtime::{self, SourcePosition, Status as RoomStatus};
use mradm_render::routing::Error as EngineError;
#[cfg(test)]
use std::ffi::CStr;
use std::ffi::{c_char, c_void};
use std::fmt::{self, Write};
use std::pin::Pin;
use std::ptr;
use std::sync::{Mutex, MutexGuard};

#[cfg(not(test))]
unsafe extern "C" {
    pub(crate) fn mradm_device_islands() -> *const DeviceIslands;
}

// Unit tests link without the C++ device islands.
#[cfg(test)]
pub(crate) unsafe fn mradm_device_islands() -> *const DeviceIslands {
    ptr::null()
}

// ── Records of `c_api.h` ─────────────────────────────────────────────────────────

/// `adm_scene_renderer_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_renderer_config_t {
    struct_size: u32,
    renderer: i32,
    output_layout: *const c_char,
    sofa_path: *const c_char,
    speaker_geometry: i32,
    speaker_spread_mode: i32,
    binaural_spread_mode: i32,
    lfe_routing_mode: i32,
    object_smoothing_frames: u32,
}

/// `adm_scene_stream_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_stream_config_t {
    struct_size: u32,
    rendering: adm_scene_renderer_config_t,
    input_sample_rate: u32,
    output_sample_rate: u32,
    input_queue_samples: u32,
    output_ring_frames: u32,
    startup_watermark_frames: u32,
    reserved_v1_35: u32,
    input_queue_bytes: u64,
}

/// `adm_scene_semantic_entity_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_semantic_entity_t {
    struct_size: u32,
    reserved_v1_35: u32,
    id: *const c_char,
    name: *const c_char,
}

/// `adm_scene_semantic_identity_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_semantic_identity_t {
    struct_size: u32,
    reserved_v1_35: u32,
    object_id: *const c_char,
    object_name: *const c_char,
    track_uid: *const c_char,
    has_importance: i32,
    importance: i32,
    has_dialogue_id: i32,
    dialogue_id: i32,
    content_count: u32,
    contents: *const adm_scene_semantic_entity_t,
    programme_count: u32,
    programmes: *const adm_scene_semantic_entity_t,
}

/// `adm_scene_element_descriptor_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_element_descriptor_t {
    struct_size: u32,
    role: i32,
    element_id: u64,
    speaker_label: *const c_char,
    flags: u64,
    has_position: i32,
    reserved_v1_35: u32,
    position_x: f32,
    position_y: f32,
    position_z: f32,
    semantic_identity: *const adm_scene_semantic_identity_t,
}

/// `adm_scene_object_state_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_object_state_t {
    struct_size: u32,
    reserved_v1_35: u32,
    valid_fields: u64,
    active: i32,
    linear_gain: f32,
    position_x: f32,
    position_y: f32,
    position_z: f32,
    extent_width: f32,
    extent_height: f32,
    extent_depth: f32,
    diffuse: f32,
    divergence: f32,
    channel_lock: i32,
    screen_reference: i32,
    head_locked: i32,
    divergence_azimuth_range: f32,
    divergence_position_range: f32,
    has_channel_lock_max_distance: i32,
    channel_lock_max_distance: f32,
}

/// `adm_scene_pcm_plane_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_pcm_plane_t {
    struct_size: u32,
    reserved_v1_35: u32,
    element_id: u64,
    samples: *const f32,
    sample_count: u32,
    stride: u32,
    has_signal: i32,
    reserved2_v1_35: u32,
}

/// `adm_scene_initial_state_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_initial_state_t {
    struct_size: u32,
    reserved_v1_35: u32,
    element_id: u64,
    state: adm_scene_object_state_t,
}

/// `adm_scene_metadata_update_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_metadata_update_t {
    struct_size: u32,
    reserved_v1_35: u32,
    element_id: u64,
    offset_samples: u32,
    ramp_duration_samples: u32,
    jump_position: i32,
    reserved2_v1_35: u32,
    changed_fields: u64,
    state: adm_scene_object_state_t,
}

/// `adm_scene_frame_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_frame_t {
    struct_size: u32,
    flags: u32,
    epoch_id: u64,
    generation_id: u64,
    media_sample_start: i64,
    duration_samples: u32,
    pcm_count: u32,
    pcm: *const adm_scene_pcm_plane_t,
    initial_state_count: u32,
    metadata_update_count: u32,
    initial_states: *const adm_scene_initial_state_t,
    metadata_updates: *const adm_scene_metadata_update_t,
}

/// `adm_scene_output_format_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_scene_output_format_t {
    struct_size: u32,
    sample_format: i32,
    sample_rate: u32,
    channels: u32,
    interleaved: i32,
    reserved_v1_35: u32,
}

/// `adm_scene_pull_result_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_scene_pull_result_t {
    struct_size: u32,
    flags: u32,
    epoch_id: u64,
    first_media_frame: u64,
    media_frames: u32,
    requested_frames: u32,
}

/// `adm_scene_stream_status_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_scene_stream_status_t {
    struct_size: u32,
    state: i32,
    epoch_id: u64,
    generation_id: u64,
    queued_input_samples: u64,
    queued_input_bytes: u64,
    buffered_output_frames: u64,
    media_frames_pulled: u64,
    underruns: u64,
    semantic_degradations: u64,
    semantic_policy_revision: u64,
    ring_fill: f32,
    ended: i32,
    failed: i32,
}

/// `adm_scene_diagnostic_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_scene_diagnostic_t {
    struct_size: u32,
    level: i32,
    code: i32,
    reserved_v1_35: u32,
    epoch_id: u64,
    generation_id: u64,
    element_id: u64,
    field_mask: u64,
    message: *const c_char,
}

/// `adm_scene_output_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_scene_output_config_t {
    struct_size: u32,
    kind: i32,
    output_layout: *const c_char,
    device_id: *const c_char,
    speaker_geometry: i32,
    reserved_v1_36: u32,
}

/// `adm_scene_output_status_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_scene_output_status_t {
    struct_size: u32,
    state: i32,
    epoch_id: u64,
    consumed_frames: u64,
    presented_frames: u64,
    queued_frames: u64,
    underruns: u64,
    clock_kind: i32,
    recovering: i32,
    failed: i32,
    ended: i32,
}

/// `adm_hptf_info_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_hptf_info_t {
    struct_size: u32,
    enabled: i32,
    band_count: u32,
    sample_rate: u32,
    preamp_db: f32,
    auto_trim_db: f32,
    max_response_db: f32,
    reserved_v1_37: u32,
    applied_revision: u64,
}

impl adm_hptf_info_t {
    /// The info of a default `HptfInfo` (bypassed, no curve).
    pub(crate) fn empty() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            enabled: 0,
            band_count: 0,
            sample_rate: 0,
            preamp_db: 0.0,
            auto_trim_db: 0.0,
            max_response_db: 0.0,
            reserved_v1_37: 0,
            applied_revision: 0,
        }
    }
}

/// `adm_hptf_stereo_info_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_hptf_stereo_info_t {
    struct_size: u32,
    reserved_v1_50: u32,
    left: adm_hptf_info_t,
    right: adm_hptf_info_t,
}

/// `adm_hptf_band_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_hptf_band_t {
    struct_size: u32,
    r#type: i32,
    enabled: i32,
    reserved_v1_38: u32,
    fc_hz: f64,
    gain_db: f64,
    q: f64,
}

/// `adm_hptf_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_hptf_config_t {
    struct_size: u32,
    profile_path: *const c_char,
    preamp_mode: i32,
    reserved_v1_37: u32,
    revision: u64,
}

/// `adm_hptf_parameters_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_hptf_parameters_t {
    struct_size: u32,
    band_count: u32,
    bands: *const adm_hptf_band_t,
    preamp_db: f64,
    preamp_mode: i32,
    reserved_v1_38: u32,
    revision: u64,
}

/// `adm_hptf_stereo_config_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_hptf_stereo_config_t {
    struct_size: u32,
    preamp_mode: i32,
    left_profile_path: *const c_char,
    right_profile_path: *const c_char,
    revision: u64,
}

/// `adm_hptf_stereo_parameters_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_hptf_stereo_parameters_t {
    struct_size: u32,
    preamp_mode: i32,
    left_band_count: u32,
    right_band_count: u32,
    left_bands: *const adm_hptf_band_t,
    right_bands: *const adm_hptf_band_t,
    left_preamp_db: f64,
    right_preamp_db: f64,
    revision: u64,
}

impl adm_hptf_stereo_info_t {
    pub(crate) fn of(left: adm_hptf_info_t, right: adm_hptf_info_t) -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            reserved_v1_50: 0,
            left,
            right,
        }
    }
}

impl adm_hptf_band_t {
    /// A parsed band with the caller's `stride` as its `struct_size`; the type as `HptfBandType`.
    pub(crate) fn of(stride: u32, band: &mradm_ffi::hptf::Band) -> Self {
        Self {
            struct_size: stride,
            r#type: i32::from(band.kind as u8),
            enabled: i32::from(band.enabled != 0),
            reserved_v1_38: 0,
            fc_hz: band.frequency,
            gain_db: band.gain_db,
            q: band.q,
        }
    }
}

// ── Shared helpers ───────────────────────────────────────────────────────────────

const RENDERER_SAF: i32 = 2;
const RENDERER_SAF_BINAURAL: i32 = 6;
const RENDERER_TRIPLE_BALANCE: i32 = 7;
const RENDERER_ROOM_BINAURAL: i32 = 8;
const ROOM_QUALITY_EXACT: i32 = 0;
const ROOM_QUALITY_REALTIME: i32 = 1;
const SUBMIT_CLOSED: i32 = 3;
const LOG_INFO: i32 = 1;
const OUTPUT_NULL: i32 = 2;
const OUTPUT_CLOCK_MEDIA: i32 = 1;
/// The output session's code for a device that threw: the C++ rethrew it.
const DEVICE_THREW: i32 = 100;

/// The `sizeof` of the C++ `live_scene` records the input budget counted (`Frame`, `PcmPlane`,
/// `StateEntry`, `MetadataUpdate`) on the 64-bit standard libraries without debug iterators.
const SIZES: (usize, usize, usize, usize) = (104, 40, 80, 120);

/// A message returned as `const char*`: empty, or the bytes with their NUL.
#[derive(Default)]
pub(crate) struct Message(Vec<u8>);

impl Message {
    pub(crate) fn as_ptr(&self) -> *const c_char {
        if self.0.is_empty() {
            c"".as_ptr()
        } else {
            self.0.as_ptr().cast()
        }
    }

    pub(crate) fn clear(&mut self) {
        self.0 = Vec::new();
    }

    /// False when the copy could not be allocated (the C++ `bad_alloc`).
    pub(crate) fn set(&mut self, text: &[u8]) -> bool {
        if text.is_empty() {
            self.clear();
            return true;
        }
        match concat(&[text, b"\0"]) {
            Ok(bytes) => {
                self.0 = bytes;
                true
            }
            Err(_) => false,
        }
    }
}

pub(crate) fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|e| e.into_inner())
}

/// A caller's optional C string; empty for NULL.
pub(crate) unsafe fn text<'a>(value: *const c_char) -> &'a [u8] {
    if value.is_null() {
        &[]
    } else {
        unsafe { c_str(value) }
    }
}

/// `make_error(code, message)`.
pub(crate) fn invalid(code: i32, message: &str) -> Failure {
    Failure::error(code, message.as_bytes(), b"")
}

/// `load_sized_array`: `count` records `stride` bytes apart, `stride` the first record's
/// `struct_size`, which every record repeats and which is at least `size_of::<T>()`. `None` for an
/// invalid array.
pub(crate) unsafe fn sized<T: Copy>(
    source: *const T,
    count: u32,
) -> Result<Option<Vec<T>>, Failure> {
    if count == 0 {
        return Ok(Some(Vec::new()));
    }
    if source.is_null() {
        return Ok(None);
    }
    let bytes = source.cast::<u8>();
    let stride = unsafe { bytes.cast::<u32>().read_unaligned() } as usize;
    if stride < size_of::<T>() || count as usize > usize::MAX / stride {
        return Ok(None);
    }
    let mut records = Vec::new();
    records
        .try_reserve_exact(count as usize)
        .map_err(|_| Failure::Exception)?;
    for index in 0..count as usize {
        let element = unsafe { bytes.add(index * stride) };
        if unsafe { element.cast::<u32>().read_unaligned() } as usize != stride {
            return Ok(None);
        }
        records.push(unsafe { element.cast::<T>().read_unaligned() });
    }
    Ok(Some(records))
}

/// `output_struct_valid`.
pub(crate) unsafe fn output_valid<T>(output: *const T) -> bool {
    !output.is_null() && unsafe { output.cast::<u32>().read_unaligned() } >= 4
}

/// `write_sized_output`: `value` with the caller's `struct_size`, up to the smaller size.
pub(crate) unsafe fn write_sized<T>(output: *mut T, value: T) {
    let caller = unsafe { output.cast::<u32>().read_unaligned() };
    let mut value = value;
    unsafe { ptr::from_mut(&mut value).cast::<u32>().write(caller) };
    let len = (caller as usize).min(size_of::<T>());
    unsafe { ptr::copy_nonoverlapping(ptr::from_ref(&value).cast::<u8>(), output.cast(), len) };
}

/// `owned_string`: a string the caller releases with `adm_free_string`.
pub(crate) unsafe fn owned(text: &[u8], out: *mut *mut c_char) -> Result<(), Failure> {
    let copy = unsafe { string::copy(text.as_ptr(), text.len()) };
    if copy.is_null() {
        return Err(Failure::Exception);
    }
    unsafe { *out = copy };
    Ok(())
}

// ── Renderer configurations ──────────────────────────────────────────────────────

/// A converted `adm_scene_renderer_config_t` (`live_scene::RendererConfig`) with the UTF-8 texts
/// its request points at.
struct Renderer {
    selection: i32,
    layout: Vec<u8>,
    sofa: Vec<u8>,
    apple_geometry: bool,
    speaker_spread: u32,
    binaural_spread: u32,
    split_power: bool,
    smoothing: u32,
    sample_rate: u32,
    reference: Option<RoomScreenGeometry>,
    reproduction: Option<RoomScreenGeometry>,
    room: Option<RoomRequest>,
}

struct RoomRequest {
    profile: *const mradm_ffi::room::RoomProfileHandle,
    hrir: Vec<u8>,
    metres: f64,
    quality: u32,
}

/// `to_scene_renderer_config` up to the SOFA path, kept as the caller's bytes
/// (`Renderer::convert_sofa`).
unsafe fn renderer_config(
    config: *const adm_scene_renderer_config_t,
    sample_rate: u32,
) -> Result<Renderer, Failure> {
    let Some(header) = (unsafe { config.cast::<u32>().as_ref() }) else {
        return Err(invalid(
            1,
            "invalid live Scene renderer configuration struct_size",
        ));
    };
    if (*header as usize) < size_of::<adm_scene_renderer_config_t>() {
        return Err(invalid(
            1,
            "invalid live Scene renderer configuration struct_size",
        ));
    }
    let input = unsafe { config.read_unaligned() };
    let renderer = input.renderer;
    if !matches!(
        renderer,
        RENDERER_SAF | RENDERER_SAF_BINAURAL | RENDERER_TRIPLE_BALANCE | RENDERER_ROOM_BINAURAL
    ) || !(0..=1).contains(&input.speaker_geometry)
        || !(0..=2).contains(&input.speaker_spread_mode)
        || !(0..=3).contains(&input.binaural_spread_mode)
        || !(0..=1).contains(&input.lfe_routing_mode)
        || input.object_smoothing_frames > 48000
    {
        return Err(invalid(
            1,
            "invalid live Scene renderer configuration value",
        ));
    }
    let binaural = renderer == RENDERER_SAF_BINAURAL || renderer == RENDERER_ROOM_BINAURAL;
    let layout = unsafe { text(input.output_layout) };
    let sofa = unsafe { text(input.sofa_path) };
    if !binaural && layout.is_empty() {
        return Err(invalid(
            1,
            "Speaker live Scene rendering requires an output layout",
        ));
    }
    if !binaural && layout == b"0+2+0" {
        return Err(invalid(
            2,
            "two-channel loudspeaker rendering is unavailable; use binaural output instead",
        ));
    }
    if !binaural && !sofa.is_empty() {
        return Err(invalid(
            2,
            "a live Scene SOFA path is valid only for SAF binaural rendering",
        ));
    }
    if binaural && !layout.is_empty() && layout != b"binaural" {
        return Err(invalid(
            2,
            "SAF binaural live Scene output layout must be 'binaural'",
        ));
    }
    Ok(Renderer {
        selection: renderer,
        layout: concat(&[if layout.is_empty() {
            b"binaural"
        } else {
            layout
        }])?,
        sofa: concat(&[sofa])?,
        apple_geometry: input.speaker_geometry == 1,
        speaker_spread: input.speaker_spread_mode as u32,
        binaural_spread: input.binaural_spread_mode as u32,
        split_power: input.lfe_routing_mode == 1,
        smoothing: input.object_smoothing_frames,
        sample_rate,
        reference: None,
        reproduction: None,
        room: None,
    })
}

impl Renderer {
    /// The SOFA path as `std::filesystem::path` held it: UTF-8 from the caller's narrow path.
    /// `None` where the C++ conversion threw.
    fn convert_sofa(&mut self) -> Option<()> {
        self.sofa = utf8_path(&self.sofa)?;
        Some(())
    }

    /// The engine's renderer configuration (it borrows `self`).
    fn config(&self, sink: bool) -> Result<RendererConfig<'_>, Failure> {
        let (Some(selection), Some(speaker_spread), Some(binaural_spread)) = (
            Selection::from_u32(self.selection as u32),
            speaker_spread_of(self.speaker_spread),
            binaural_spread_of(self.binaural_spread),
        ) else {
            return Err(invalid(1, "无效 Scene 流式渲染参数"));
        };
        let reference = self.reference.unwrap_or_default();
        let room = self.room.as_ref();
        Ok(RendererConfig {
            selection,
            layout_id: &self.layout,
            apple_geometry: self.apple_geometry,
            speaker_spread,
            binaural_spread,
            split_power: self.split_power,
            smoothing_frames: self.smoothing,
            sample_rate: self.sample_rate,
            sofa: (!self.sofa.is_empty()).then(|| Sofa {
                path: &self.sofa,
                file_name: file_name(&self.sofa),
            }),
            screens: Screens {
                has_reference: self.reference.is_some(),
                reference,
                has_reproduction: self.reproduction.is_some(),
                reproduction: self.reproduction.unwrap_or(reference),
            },
            room: EngineRoomRequest {
                package: room
                    .and_then(|r| unsafe { r.profile.as_ref() })
                    .map(mradm_ffi::room::RoomProfileHandle::package),
                hrir: room.map_or(&[][..], |r| &r.hrir[..]),
                metres: room.map_or(0.0, |r| r.metres),
                quality: room.map_or(0, |r| r.quality),
            },
            sink,
        })
    }
}

/// An engine error as the C ABI stores it.
pub(crate) fn engine_failure(error: EngineError) -> Failure {
    match Error::new(error.code, &error.message, &error.context) {
        Ok(error) => error.into(),
        Err(failure) => failure,
    }
}

/// `parse_screen_config_json`: the screens as `ScreenGeometry` would hold them.
pub(crate) unsafe fn screens(
    source: &[u8],
) -> Result<(Option<RoomScreenGeometry>, Option<RoomScreenGeometry>), Failure> {
    if source.len() > 1024 * 1024 {
        return Err(invalid(1, "屏幕配置超过 1 MiB"));
    }
    let parsed = mradm_scene::screen::parse(source)
        .map_err(|error| Failure::error(error.code, error.message.as_bytes(), b""))?;
    let normal = |s: RoomScreenGeometry| RoomScreenGeometry {
        cartesian: u32::from(s.cartesian != 0),
        reserved: 0,
        ..s
    };
    let reference = parsed.reference.map(normal);
    let reproduction = parsed.reproduction.map(normal);
    // `validate_screen_config`.
    let default = reference.unwrap_or_default();
    let validated = RoomScreens {
        enabled: 1,
        reserved: 0,
        reference: default,
        reproduction: reproduction.unwrap_or(default),
    };
    if let Err(error) = mradm_ffi::room::validate_screens(&[validated]) {
        let (code, message) = mradm_ffi::room::error_code(&error);
        return Err(Error::new(code, message.as_bytes(), b"screen")?.into());
    }
    Ok((reference, reproduction))
}

// ── Scene streams ────────────────────────────────────────────────────────────────

#[derive(Default)]
struct StreamMessages {
    last_error: Message,
    /// Backs `adm_scene_diagnostic_t::message` until another non-pull stream call.
    diagnostic: Message,
}

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_scene_stream_t {
    handle: Box<EngineSceneStream>,
    output_rate: u32,
    channels: u32,
    input_rate: u32,
    messages: Mutex<StreamMessages>,
}

// SAFETY: The engine handle is thread-safe (the C++ shell was shared across threads).
unsafe impl Send for adm_scene_stream_t {}
unsafe impl Sync for adm_scene_stream_t {}

impl adm_scene_stream_t {
    fn stream(&self) -> &engine_stream::Stream {
        self.handle.stream()
    }

    /// The handle the not-yet-direct room and output entries take.
    fn raw(&self) -> *const EngineSceneStream {
        &*self.handle
    }

    fn clear(&self) {
        let mut m = lock(&self.messages);
        m.last_error.clear();
        m.diagnostic.clear();
    }

    fn store_message(&self, message: &[u8]) -> bool {
        lock(&self.messages).last_error.set(message)
    }

    /// The result of a stream export body: its code, an error stored as `message: locator`, or an
    /// exception stored as `unexpected`.
    fn report(&self, result: Result<i32, Failure>, unexpected: &str) -> i32 {
        match result {
            Ok(code) => code,
            Err(Failure::Error(e)) => {
                let separator = if e.context.is_empty() {
                    &b""[..]
                } else {
                    b": "
                };
                if concat(&[&e.message, separator, &e.context])
                    .is_ok_and(|text| self.store_message(&text))
                {
                    error::map(e.code)
                } else {
                    self.store_message(unexpected.as_bytes());
                    INTERNAL
                }
            }
            Err(Failure::Exception) => {
                self.store_message(unexpected.as_bytes());
                INTERNAL
            }
        }
    }

    /// Clears the messages and runs `body` as the C++ `try` did.
    fn call(&self, unexpected: &str, body: impl FnOnce() -> Result<i32, Failure>) -> i32 {
        let result = guard::guard(Err(Failure::Exception), || {
            self.clear();
            body()
        });
        self.report(result, unexpected)
    }

    fn attached(&self) -> bool {
        self.stream().output_attached()
    }
}

unsafe fn stream_ref<'a>(stream: *const adm_scene_stream_t) -> Option<&'a adm_scene_stream_t> {
    unsafe { stream.as_ref() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_scene_stream(
    context: *mut adm_context_t,
    config: *const adm_scene_stream_config_t,
    out: *mut *mut adm_scene_stream_t,
) -> i32 {
    unsafe { adm_create_scene_stream_with_room(context, config, ptr::null(), out) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_scene_stream_with_room(
    context: *mut adm_context_t,
    config: *const adm_scene_stream_config_t,
    room: *const adm_room_config_t,
    out: *mut *mut adm_scene_stream_t,
) -> i32 {
    unsafe { adm_create_scene_stream_with_room_ex(context, config, room, ptr::null(), out) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_scene_stream_with_room_ex(
    context: *mut adm_context_t,
    config: *const adm_scene_stream_config_t,
    room: *const adm_room_config_t,
    screen_json: *const c_char,
    out: *mut *mut adm_scene_stream_t,
) -> i32 {
    unsafe {
        adm_create_scene_stream_with_room_quality(
            context,
            config,
            room,
            ROOM_QUALITY_EXACT,
            screen_json,
            out,
        )
    }
}

const CREATE_UNEXPECTED: &str = "unexpected exception while creating live Scene stream";
/// The code of an error whose message is the catch-all's: stored as is, returned as INTERNAL.
const UNEXPECTED: i32 = -1;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_scene_stream_with_room_quality(
    context: *mut adm_context_t,
    config: *const adm_scene_stream_config_t,
    room: *const adm_room_config_t,
    quality: i32,
    screen_json: *const c_char,
    out: *mut *mut adm_scene_stream_t,
) -> i32 {
    if let Some(out) = unsafe { out.as_mut() } {
        *out = ptr::null_mut();
    }
    let header = unsafe { config.cast::<u32>().as_ref() };
    if context.is_null()
        || out.is_null()
        || header.is_none_or(|&size| (size as usize) < size_of::<adm_scene_stream_config_t>())
        || !(quality == ROOM_QUALITY_EXACT || quality == ROOM_QUALITY_REALTIME)
    {
        return INVALID_ARGUMENT;
    }
    let result = guard::guard(Err(Failure::Exception), || unsafe {
        error::clear(context);
        create(config, room, quality, screen_json, out)
    });
    match result {
        Ok(code) => code,
        Err(failure @ Failure::Error(_)) => unsafe { error::report(context, failure, None) },
        Err(Failure::Exception) => {
            if let Ok(message) = concat(&[CREATE_UNEXPECTED.as_bytes(), b": ", BAD_ALLOC]) {
                unsafe { error::set(context, &message) };
            }
            INTERNAL
        }
    }
}

unsafe fn create(
    config: *const adm_scene_stream_config_t,
    room: *const adm_room_config_t,
    quality: i32,
    screen_json: *const c_char,
    out: *mut *mut adm_scene_stream_t,
) -> Result<i32, Failure> {
    let input = unsafe { config.read_unaligned() };
    let mut renderer = unsafe { renderer_config(&input.rendering, input.input_sample_rate)? };
    if renderer.convert_sofa().is_none() {
        // The C++ `std::filesystem::path` threw: its catch stored the exception's text.
        let reason: &[u8] = if cfg!(windows) {
            "路径无法转换为 UTF-8".as_bytes()
        } else {
            BAD_ALLOC
        };
        return match concat(&[CREATE_UNEXPECTED.as_bytes(), b": ", reason]) {
            Ok(message) => Err(Failure::Error(Error {
                code: UNEXPECTED,
                message,
                context: Vec::new(),
            })),
            Err(failure) => Err(failure),
        };
    }
    let room_renderer = renderer.selection == RENDERER_ROOM_BINAURAL;
    if quality != ROOM_QUALITY_EXACT && !room_renderer {
        return Ok(UNSUPPORTED);
    }
    if !screen_json.is_null() {
        if !room_renderer {
            return Ok(UNSUPPORTED);
        }
        (renderer.reference, renderer.reproduction) = unsafe { screens(c_str(screen_json))? };
    }
    if !room.is_null() || room_renderer {
        let config = unsafe { room::config(room)? };
        if !room_renderer {
            return Ok(INVALID_ARGUMENT);
        }
        renderer.room = Some(RoomRequest {
            profile: config.profile,
            hrir: concat(&[config.hrir])?,
            metres: config.metres,
            quality: quality as u32,
        });
    }
    let (frame, plane, entry, update) = SIZES;
    let config = StreamConfig {
        output_sample_rate: input.output_sample_rate,
        input_queue_samples: input.input_queue_samples,
        input_queue_bytes: input.input_queue_bytes,
        output_ring_frames: input.output_ring_frames,
        startup_watermark_frames: input.startup_watermark_frames,
        manual_clock: None,
        sizes: Sizes {
            frame,
            plane,
            entry,
            update,
        },
    };
    let handle = create_stream(&config, &renderer.config(true)?).map_err(engine_failure)?;
    let (output_rate, channels) = handle.stream().output_format();
    let input_rate = handle.stream().input_sample_rate();
    let handle = error::try_box(adm_scene_stream_t {
        handle,
        output_rate,
        channels,
        input_rate,
        messages: Mutex::new(StreamMessages::default()),
    });
    if handle.is_null() {
        // The value dropped with the failed allocation destroyed the engine.
        return Err(Failure::Exception);
    }
    unsafe { *out = handle };
    Ok(OK)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_scene_stream(stream: *mut adm_scene_stream_t) {
    if !stream.is_null() {
        guard::unit(|| drop(unsafe { Box::from_raw(stream) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_last_error_message(
    stream: *const adm_scene_stream_t,
) -> *const c_char {
    match unsafe { stream_ref(stream) } {
        Some(s) => lock(&s.messages).last_error.as_ptr(),
        None => c"".as_ptr(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_get_output_format(
    stream: *mut adm_scene_stream_t,
    out: *mut adm_scene_output_format_t,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if !unsafe { output_valid(out) } {
        return INVALID_ARGUMENT;
    }
    s.clear();
    unsafe {
        write_sized(
            out,
            adm_scene_output_format_t {
                struct_size: 0,
                sample_format: 0,
                sample_rate: s.output_rate,
                channels: s.channels,
                interleaved: 1,
                reserved_v1_35: 0,
            },
        );
    }
    OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_begin_epoch(
    stream: *mut adm_scene_stream_t,
    epoch_id: u64,
    target_sample: i64,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if s.attached() {
        return INVALID_ARGUMENT;
    }
    s.call(
        "unexpected exception while beginning live Scene epoch",
        || {
            s.stream()
                .begin_epoch(epoch_id, target_sample)
                .map_err(engine_failure)?;
            Ok(OK)
        },
    )
}

/// The entities of a membership array; `None` for an invalid one.
unsafe fn entities(
    source: *const adm_scene_semantic_entity_t,
    count: u32,
) -> Result<Option<Vec<Entity>>, Failure> {
    let Some(items) = (unsafe { sized(source, count)? }) else {
        return Ok(None);
    };
    let mut records = reserve(items.len())?;
    for e in &items {
        records.push(Entity {
            id: concat(&[unsafe { text(e.id) }])?,
            name: concat(&[unsafe { text(e.name) }])?,
        });
    }
    Ok(Some(records))
}

/// `to_scene_semantic_identity`; `None` for an invalid identity.
unsafe fn identity(
    source: &adm_scene_semantic_identity_t,
) -> Result<Option<EngineIdentity>, Failure> {
    if (source.struct_size as usize) < size_of::<adm_scene_semantic_identity_t>()
        || (source.has_importance != 0 && !(0..=10).contains(&source.importance))
        || (source.has_dialogue_id != 0 && !(0..=2).contains(&source.dialogue_id))
    {
        return Ok(None);
    }
    let (Some(contents), Some(programmes)) = (
        unsafe { entities(source.contents, source.content_count)? },
        unsafe { entities(source.programmes, source.programme_count)? },
    ) else {
        return Ok(None);
    };
    Ok(Some(EngineIdentity {
        object_id: concat(&[unsafe { text(source.object_id) }])?,
        object_name: concat(&[unsafe { text(source.object_name) }])?,
        track_uid: concat(&[unsafe { text(source.track_uid) }])?,
        importance: (source.has_importance != 0).then_some(source.importance),
        dialogue_id: (source.has_dialogue_id != 0).then_some(source.dialogue_id),
        contents,
        programmes,
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_configure_generation(
    stream: *mut adm_scene_stream_t,
    epoch_id: u64,
    generation_id: u64,
    elements: *const adm_scene_element_descriptor_t,
    element_count: u32,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    s.call(
        "unexpected exception while configuring live Scene generation",
        || {
            let Some(borrowed) = (unsafe { sized(elements, element_count)? }) else {
                return Err(invalid(
                    1,
                    "invalid live Scene element descriptor array/stride",
                ));
            };
            let mut descriptors: Vec<Descriptor> = reserve(borrowed.len())?;
            for d in &borrowed {
                if !(0..=2).contains(&d.role) {
                    return Err(invalid(1, "invalid live Scene element role"));
                }
                let identity = match unsafe { d.semantic_identity.as_ref() } {
                    None => None,
                    Some(source) => match unsafe { identity(source)? } {
                        Some(found) => Some(found),
                        None => {
                            return Err(invalid(
                                1,
                                "invalid live Scene semantic identity or membership array",
                            ));
                        }
                    },
                };
                descriptors.push(Descriptor {
                    element_id: d.element_id,
                    role: d.role as u32,
                    speaker_label: concat(&[unsafe { text(d.speaker_label) }])?,
                    has_position: d.has_position != 0,
                    position: [d.position_x, d.position_y, d.position_z],
                    flags: d.flags,
                    identity,
                });
            }
            s.stream()
                .configure_generation(epoch_id, generation_id, &descriptors)
                .map_err(engine_failure)?;
            Ok(OK)
        },
    )
}

/// `to_scene_state`.
pub(crate) fn state(s: &adm_scene_object_state_t) -> live::State {
    live::State {
        valid_fields: s.valid_fields,
        active: s.active != 0,
        linear_gain: s.linear_gain,
        position: [s.position_x, s.position_y, s.position_z],
        width: s.extent_width,
        height: s.extent_height,
        depth: s.extent_depth,
        diffuse: s.diffuse,
        divergence: s.divergence,
        channel_lock: s.channel_lock != 0,
        screen_reference: s.screen_reference != 0,
        head_locked: s.head_locked != 0,
        divergence_azimuth_range: s.divergence_azimuth_range,
        divergence_position_range: s.divergence_position_range,
        channel_lock_max_distance: (s.has_channel_lock_max_distance != 0)
            .then_some(s.channel_lock_max_distance),
    }
}

/// The producer's planes, read in place.
struct Planes<'a>(&'a [adm_scene_pcm_plane_t]);

impl PlaneInputs for Planes<'_> {
    fn len(&self) -> usize {
        self.0.len()
    }
    fn plane(&self, plane: usize) -> PlaneInput {
        let p = &self.0[plane];
        PlaneInput {
            element_id: p.element_id,
            sample_count: p.sample_count,
            stride: p.stride,
            has_signal: p.has_signal != 0,
            has_samples: !p.samples.is_null(),
        }
    }
    fn sample(&self, plane: usize, index: usize) -> f32 {
        let p = &self.0[plane];
        // The engine reads a sample only after the plane passed its checks: signalled, non-null,
        // a non-zero stride whose span fits the address space, and `index` below the frame
        // duration. The caller's view holds those samples (the C ABI contract).
        unsafe { p.samples.add(index * p.stride as usize).read() }
    }
}

pub(crate) fn reserve<T>(len: usize) -> Result<Vec<T>, Failure> {
    let mut items = Vec::new();
    items
        .try_reserve_exact(len)
        .map_err(|_| Failure::Exception)?;
    Ok(items)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_submit_frame(
    stream: *mut adm_scene_stream_t,
    frame: *const adm_scene_frame_t,
    timeout_ms: u32,
    out_submit_status: *mut i32,
) -> i32 {
    if let Some(status) = unsafe { out_submit_status.as_mut() } {
        *status = SUBMIT_CLOSED;
    }
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    let header = unsafe { frame.cast::<u32>().as_ref() };
    if out_submit_status.is_null()
        || header.is_none_or(|&size| (size as usize) < size_of::<adm_scene_frame_t>())
    {
        return INVALID_ARGUMENT;
    }
    s.call(
        "unexpected exception while submitting live Scene frame",
        || {
            let input = unsafe { frame.read_unaligned() };
            let (Some(pcm), Some(initial_states), Some(updates)) = (
                unsafe { sized(input.pcm, input.pcm_count)? },
                unsafe { sized(input.initial_states, input.initial_state_count)? },
                unsafe { sized(input.metadata_updates, input.metadata_update_count)? },
            ) else {
                return Err(invalid(1, "invalid live Scene frame array pointer/stride"));
            };
            let state_size = size_of::<adm_scene_object_state_t>();
            let mut initial = reserve(initial_states.len())?;
            for e in &initial_states {
                if (e.state.struct_size as usize) < state_size {
                    return Err(invalid(1, "invalid live Scene initial-state struct_size"));
                }
                initial.push(live::Entry {
                    element_id: e.element_id,
                    state: state(&e.state),
                });
            }
            let mut targets = reserve(updates.len())?;
            for u in &updates {
                if (u.state.struct_size as usize) < state_size {
                    return Err(invalid(1, "invalid live Scene metadata target struct_size"));
                }
                targets.push(live::Update {
                    element_id: u.element_id,
                    offset: u.offset_samples,
                    ramp: u.ramp_duration_samples,
                    jump: u.jump_position != 0,
                    changed: u.changed_fields,
                    cleared: 0,
                    state: state(&u.state),
                });
            }
            let planes = Planes(&pcm);
            let view = FrameInput {
                epoch: input.epoch_id,
                generation: input.generation_id,
                start: input.media_sample_start,
                duration: input.duration_samples,
                flags: input.flags,
                planes: &planes,
                initial: &initial,
                updates: &targets,
            };
            let submitted = s
                .stream()
                .submit_frame(&view, i64::from(timeout_ms))
                .map_err(engine_failure)?;
            unsafe { *out_submit_status = submitted as i32 };
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_signal_end(
    stream: *mut adm_scene_stream_t,
    epoch_id: u64,
    end_sample: i64,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    s.call("unexpected exception while ending live Scene epoch", || {
        s.stream()
            .signal_end(epoch_id, end_sample)
            .map_err(engine_failure)?;
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_pull(
    stream: *mut adm_scene_stream_t,
    interleaved_output: *mut f32,
    frames: u32,
    result: *mut adm_scene_pull_result_t,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if s.attached()
        || !unsafe { output_valid(result) }
        || (frames != 0 && interleaved_output.is_null())
    {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let needed = frames as usize * s.channels as usize;
        let output = if needed == 0 {
            &mut [][..]
        } else {
            // Checked above: a non-null buffer the caller sized for `frames` frames.
            unsafe { std::slice::from_raw_parts_mut(interleaved_output, needed) }
        };
        let pulled = s.stream().pull(output, frames);
        unsafe {
            write_sized(
                result,
                adm_scene_pull_result_t {
                    struct_size: 0,
                    flags: pulled.flags,
                    epoch_id: pulled.epoch,
                    first_media_frame: pulled.first_media_frame,
                    media_frames: pulled.media_frames,
                    requested_frames: pulled.requested_frames,
                },
            );
        }
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_get_status(
    stream: *mut adm_scene_stream_t,
    out: *mut adm_scene_stream_status_t,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if !unsafe { output_valid(out) } {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let v = s.stream().status();
        unsafe {
            write_sized(
                out,
                adm_scene_stream_status_t {
                    struct_size: 0,
                    state: v.state as i32,
                    epoch_id: v.epoch,
                    generation_id: v.generation,
                    queued_input_samples: v.queued_input_samples,
                    queued_input_bytes: v.queued_input_bytes,
                    buffered_output_frames: v.buffered_output_frames,
                    media_frames_pulled: v.media_frames_pulled,
                    underruns: v.underruns,
                    semantic_degradations: v.semantic_degradations,
                    semantic_policy_revision: v.semantic_policy_revision,
                    ring_fill: v.ring_fill,
                    ended: i32::from(v.ended),
                    failed: i32::from(v.failed),
                },
            );
        }
        OK
    })
}

/// `switch_backend` of a converted configuration.
unsafe fn switch_backend(
    s: &adm_scene_stream_t,
    config: *const adm_scene_renderer_config_t,
) -> Result<(), Failure> {
    let mut renderer = unsafe { renderer_config(config, s.input_rate)? };
    renderer.convert_sofa().ok_or(Failure::Exception)?;
    s.stream()
        .switch_backend(&renderer.config(true)?)
        .map_err(engine_failure)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_switch_backend(
    stream: *mut adm_scene_stream_t,
    config: *const adm_scene_renderer_config_t,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    s.call(
        "unexpected exception while switching live Scene backend",
        || {
            unsafe { switch_backend(s, config)? };
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_set_listener_orientation(
    stream: *mut adm_scene_stream_t,
    yaw_deg: f32,
    pitch_deg: f32,
    roll_deg: f32,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if !yaw_deg.is_finite() || !pitch_deg.is_finite() || !roll_deg.is_finite() {
        return INVALID_ARGUMENT;
    }
    s.call(
        "unexpected exception while setting live Scene listener orientation",
        || {
            s.stream()
                .set_listener_orientation([yaw_deg, pitch_deg, roll_deg]);
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_switch_backend_ex(
    stream: *const adm_scene_stream_t,
    config: *const adm_scene_renderer_config_t,
    out_error: *mut *mut c_char,
) -> i32 {
    if out_error.is_null() {
        return INVALID_ARGUMENT;
    }
    unsafe { *out_error = ptr::null_mut() };
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| match unsafe { switch_backend(s, config) } {
        Ok(()) => OK,
        Err(Failure::Error(e)) => match unsafe { owned(&e.message, out_error) } {
            Ok(()) => error::map(e.code),
            Err(_) => INTERNAL,
        },
        Err(Failure::Exception) => INTERNAL,
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_set_semantic_policy_json(
    stream: *mut adm_scene_stream_t,
    json: *const c_char,
    revision: u64,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    s.call(
        "unexpected exception while setting live Scene semantic policy",
        || {
            s.stream()
                .set_semantic_policy_json(unsafe { text(json) }, revision)
                .map_err(engine_failure)?;
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_log_count(stream: *mut adm_scene_stream_t) -> u32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return 0;
    };
    guard::guard(0, || {
        let mut m = lock(&s.messages);
        m.diagnostic.clear();
        let count = s.stream().diagnostic_count();
        u32::try_from(count).unwrap_or(u32::MAX)
    })
}

/// `to_c_log_level`.
fn log_level(level: u32) -> i32 {
    if level <= 3 { level as i32 } else { LOG_INFO }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_log_entry(
    stream: *mut adm_scene_stream_t,
    index: u32,
    out: *mut adm_scene_diagnostic_t,
) -> i32 {
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return 0;
    };
    if !unsafe { output_valid(out) } {
        return 0;
    }
    guard::guard(0, || {
        let mut m = lock(&s.messages);
        m.diagnostic.clear();
        let Some(record) = s.stream().diagnostic(index as usize) else {
            return 0;
        };
        if !m.diagnostic.set(&record.message) {
            return 0;
        }
        unsafe {
            write_sized(
                out,
                adm_scene_diagnostic_t {
                    struct_size: 0,
                    level: log_level(record.level),
                    code: record.code as i32,
                    reserved_v1_35: 0,
                    epoch_id: record.epoch,
                    generation_id: record.generation,
                    element_id: record.element,
                    field_mask: record.field,
                    message: m.diagnostic.as_ptr(),
                },
            );
        }
        1
    })
}

// ── Scene stream rooms ───────────────────────────────────────────────────────────

/// A room control handle of a Scene stream's or a monitor's current backend.
pub(crate) struct Control(room_runtime::Control);

impl Control {
    /// The control behind a private handle; `None` for null (a backend without a room renderer).
    pub(crate) fn new(raw: *mut RenderRoomControl) -> Option<Self> {
        // SAFETY: `raw` comes straight from a `*_room_control` entry and is used only here.
        unsafe { RenderRoomControl::take(raw) }.map(Self)
    }
}

fn control(s: &adm_scene_stream_t) -> Option<Control> {
    s.stream().room_control().map(Control)
}

fn room_status(control: &Control) -> RoomStatus {
    control.0.status()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_switch_room(
    stream: *const adm_scene_stream_t,
    config: *const adm_room_config_t,
    out_request: *mut u64,
    out_error: *mut *mut c_char,
) -> i32 {
    if let Some(r) = unsafe { out_request.as_mut() } {
        *r = 0;
    }
    if let Some(e) = unsafe { out_error.as_mut() } {
        *e = ptr::null_mut();
    }
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if out_request.is_null() || out_error.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| unsafe { switch_room(control(s), config, out_request, out_error) })
}

/// `room_switch` once the outputs are checked and cleared: request the room of `config` at the
/// current quality.
pub(crate) unsafe fn switch_room(
    control: Option<Control>,
    config: *const adm_room_config_t,
    out_request: *mut u64,
    out_error: *mut *mut c_char,
) -> i32 {
    {
        let Some(control) = control else {
            return UNSUPPORTED;
        };
        let fail = |e: Error| match unsafe { owned(&e.message, out_error) } {
            Ok(()) => error::map(e.code),
            Err(_) => INTERNAL,
        };
        let config = match unsafe { room::config(config) } {
            Ok(config) => config,
            Err(Failure::Error(e)) => return fail(e),
            Err(Failure::Exception) => return INTERNAL,
        };
        let quality = room_status(&control).quality;
        // SAFETY: `room::config` checked the room handle the caller passed.
        let profile = unsafe { config.profile.as_ref() };
        let revision = match switch_config(profile, config.hrir, config.metres, quality)
            .and_then(|config| control.0.request(&config))
        {
            Ok(revision) => revision,
            Err(e) => {
                return match Error::new(e.code, &e.message, b"") {
                    Ok(e) => fail(e),
                    Err(_) => INTERNAL,
                };
            }
        };
        unsafe { *out_request = revision };
        OK
    }
}

/// A JSON sink whose growth can fail without aborting the host. The scalar writers allocate
/// only through this sink; no owned JSON tree or temporary formatted strings are needed.
#[derive(Default)]
struct JsonText(String);

impl Write for JsonText {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        self.0.try_reserve(text.len()).map_err(|_| fmt::Error)?;
        self.0.push_str(text);
        Ok(())
    }
}

/// `room_status_json`'s document; `None` where nlohmann threw (an unknown state, invalid UTF-8,
/// or an allocation failure). Keep keys in the former map's byte order.
fn status_json(status: &RoomStatus) -> Option<String> {
    const STATES: [&str; 5] = [
        "applied",
        "preparing",
        "transitioning",
        "draining",
        "failed",
    ];
    let state = *STATES.get(status.state as usize)?;
    let quality = match status.quality as u32 {
        0 => "exact",
        1 => "realtime",
        _ => "unknown",
    };
    let message = status.message.as_str();
    let mut json = JsonText::default();
    write!(
        json,
        "{{\"applied_revision\":{},\"buffer_frames\":{},\"constrained_sources\":{},\
         \"material_filter_guard_frames\":{},\"material_frames\":{},\"message\":",
        status.applied_revision,
        status.buffer_frames,
        status.constrained_sources,
        status.material_filter_guard_frames,
        status.material_frames,
    )
    .ok()?;
    mradm_json::write_string(&mut json, message).ok()?;
    write!(
        json,
        ",\"quality\":\"{quality}\",\"reflection_update_frames\":{},\"requested_revision\":{},\
         \"schema\":\"mradm.room-status\",\"state\":\"{state}\",\"tail_frames\":{},\
         \"transition_sample\":{},\"version\":1}}",
        status.reflection_update_frames,
        status.requested_revision,
        status.tail_frames,
        status.transition_sample,
    )
    .ok()?;
    Some(json.0)
}

fn source_json(id: u64, source: &SourcePosition) -> Option<String> {
    let triple = |json: &mut JsonText, values: [f64; 3]| -> fmt::Result {
        json.write_char('[')?;
        for (index, value) in values.into_iter().enumerate() {
            if index != 0 {
                json.write_char(',')?;
            }
            mradm_json::write_f64(json, value)?;
        }
        json.write_char(']')
    };
    let mut json = JsonText::default();
    write!(
        json,
        "{{\"constrained\":{},\"direction\":",
        source.constrained
    )
    .ok()?;
    triple(&mut json, source.direction).ok()?;
    json.write_str(",\"distance_metres\":").ok()?;
    mradm_json::write_f64(&mut json, source.distance).ok()?;
    write!(json, ",\"element_id\":{id},\"position_metres\":").ok()?;
    triple(&mut json, source.position).ok()?;
    write!(
        json,
        ",\"present\":{},\"schema\":\"mradm.room-source\",\"version\":1}}",
        source.present,
    )
    .ok()?;
    Some(json.0)
}

/// Publish only a complete document, including when the final ABI-owned copy cannot allocate.
unsafe fn room_json(out: *mut *mut c_char, build: impl FnOnce() -> Option<String>) -> i32 {
    unsafe { *out = ptr::null_mut() };
    let Some(json) = build() else {
        return INTERNAL;
    };
    match unsafe { owned(json.as_bytes(), out) } {
        Ok(()) => OK,
        Err(_) => INTERNAL,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_room_status_json(
    stream: *const adm_scene_stream_t,
    out_json: *mut *mut c_char,
) -> i32 {
    if let Some(out) = unsafe { out_json.as_mut() } {
        *out = ptr::null_mut();
    }
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if out_json.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| unsafe { room_status_json(control(s), out_json) })
}

/// `room_status_json` once the output is checked and cleared.
pub(crate) unsafe fn room_status_json(control: Option<Control>, out_json: *mut *mut c_char) -> i32 {
    let Some(control) = control else {
        return UNSUPPORTED;
    };
    unsafe { room_json(out_json, || status_json(&room_status(&control))) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_stream_room_source_json(
    stream: *const adm_scene_stream_t,
    element_id: u64,
    out_json: *mut *mut c_char,
) -> i32 {
    if let Some(out) = unsafe { out_json.as_mut() } {
        *out = ptr::null_mut();
    }
    let Some(s) = (unsafe { stream_ref(stream) }) else {
        return INVALID_ARGUMENT;
    };
    if out_json.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| unsafe { room_source_json(control(s), element_id, out_json) })
}

/// `room_source_json` once the output is checked and cleared.
pub(crate) unsafe fn room_source_json(
    control: Option<Control>,
    element_id: u64,
    out_json: *mut *mut c_char,
) -> i32 {
    let Some(control) = control else {
        return UNSUPPORTED;
    };
    let source = control.0.source_position(element_id);
    unsafe { room_json(out_json, || source_json(element_id, &source)) }
}

// ── Scene outputs ────────────────────────────────────────────────────────────────

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_scene_output_t {
    /// Pinned: the device callback holds the session's address.
    handle: Pin<Box<EngineSceneOutput>>,
    /// Serialises the calls and holds the last error message.
    message: Mutex<Message>,
}

// SAFETY: The output session serialises its own control calls.
unsafe impl Send for adm_scene_output_t {}
unsafe impl Sync for adm_scene_output_t {}

/// `scene_output_call`: under the output's lock, `action`'s error message (without its locator)
/// or nothing becomes the last error message.
fn output_call(
    output: *const adm_scene_output_t,
    action: impl FnOnce(&EngineSceneOutput) -> Result<(), Failure>,
) -> i32 {
    let Some(o) = (unsafe { output.as_ref() }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| {
        let mut message = lock(&o.message);
        match action(&o.handle) {
            Ok(()) => {
                message.clear();
                OK
            }
            Err(Failure::Error(e)) => {
                if message.set(&e.message) {
                    error::map(e.code)
                } else {
                    INTERNAL
                }
            }
            Err(Failure::Exception) => INTERNAL,
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_scene_output(
    context: *mut adm_context_t,
    stream: *const adm_scene_stream_t,
    config: *const adm_scene_output_config_t,
    out: *mut *mut adm_scene_output_t,
) -> i32 {
    if let Some(out) = unsafe { out.as_mut() } {
        *out = ptr::null_mut();
    }
    let header = unsafe { config.cast::<u32>().as_ref() };
    if context.is_null()
        || stream.is_null()
        || out.is_null()
        || header.is_none_or(|&size| (size as usize) < size_of::<adm_scene_output_config_t>())
    {
        return INVALID_ARGUMENT;
    }
    let c = unsafe { config.read_unaligned() };
    if !(0..=OUTPUT_NULL).contains(&c.kind) || !(0..=1).contains(&c.speaker_geometry) {
        return INVALID_ARGUMENT;
    }
    let s = unsafe { &*stream };
    let result = guard::guard(Err(Failure::Exception), || {
        let layout = unsafe { text(c.output_layout) };
        let device_id = unsafe { text(c.device_id) };
        // SAFETY: The device island table is immutable process data.
        let opened = unsafe {
            EngineSceneOutput::open(
                &*s.raw(),
                c.kind as u32,
                layout,
                c.speaker_geometry as u32,
                device_id,
                mradm_device_islands(),
            )
        };
        let handle = match opened {
            Ok(handle) => handle,
            // The device threw: the C++ shell rethrew it.
            Err(e) if e.code == DEVICE_THREW => return Err(Failure::Exception),
            Err(e) => return Err(engine_failure(e)),
        };
        let output = error::try_box(adm_scene_output_t {
            handle,
            message: Mutex::new(Message::default()),
        });
        if output.is_null() {
            // The value dropped with the failed allocation closed the session.
            return Err(Failure::Exception);
        }
        unsafe { *out = output };
        Ok(OK)
    });
    match result {
        Ok(code) => code,
        Err(failure) => unsafe { error::report(context, failure, None) },
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_scene_output(output: *mut adm_scene_output_t) {
    if !output.is_null() {
        guard::unit(|| drop(unsafe { Box::from_raw(output) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_last_error_message(
    output: *const adm_scene_output_t,
) -> *const c_char {
    match unsafe { output.as_ref() } {
        Some(o) => lock(&o.message).as_ptr(),
        None => c"".as_ptr(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_play(output: *mut adm_scene_output_t) -> i32 {
    output_call(output, |h| {
        h.play();
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_pause(output: *mut adm_scene_output_t) -> i32 {
    output_call(output, |h| {
        h.pause();
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_begin_epoch(
    output: *mut adm_scene_output_t,
    epoch: u64,
    target_sample: i64,
) -> i32 {
    output_call(output, |h| {
        h.begin_epoch(epoch, target_sample).map_err(engine_failure)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_volume(
    output: *mut adm_scene_output_t,
    gain: f32,
) -> i32 {
    output_call(output, |h| h.set_volume(gain).map_err(engine_failure))
}

/// `apply_hptf_request`: the caller's request of `kind` (0 file, 1 parameters, 2 stereo files,
/// 3 stereo parameters) as Rust settings, designed and published for the output.
pub(crate) unsafe fn apply_hptf(
    output: &EngineSceneOutput,
    config: *const c_void,
    kind: u32,
) -> Result<(), Failure> {
    let settings = unsafe { hptf_settings(config, kind)? };
    output.set_hptf_settings(&settings).map_err(engine_failure)
}

/// The caller's HpTF request of `kind` as Rust settings.
pub(crate) unsafe fn hptf_settings(config: *const c_void, kind: u32) -> Result<Settings, Failure> {
    // SAFETY: `config` is the caller's request of `kind` (the C ABI contract of the setters).
    unsafe { Settings::read(config, kind) }
        .map_err(|(code, message)| Failure::error(code, message.as_bytes(), b""))
}

fn scene_hptf(output: *const adm_scene_output_t, config: *const c_void, kind: u32) -> i32 {
    output_call(output, |h| unsafe { apply_hptf(h, config, kind) })
}

unsafe fn scene_hptf_ex(
    output: *const adm_scene_output_t,
    config: *const c_void,
    kind: u32,
    out_error: *mut *mut c_char,
) -> i32 {
    if out_error.is_null() {
        return INVALID_ARGUMENT;
    }
    unsafe { *out_error = ptr::null_mut() };
    let Some(o) = (unsafe { output.as_ref() }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| match unsafe { apply_hptf(&o.handle, config, kind) } {
        Ok(()) => OK,
        Err(Failure::Error(e)) => match unsafe { owned(&e.message, out_error) } {
            Ok(()) => error::map(e.code),
            Err(_) => INTERNAL,
        },
        Err(Failure::Exception) => INTERNAL,
    })
}

/// `HptfCoefficients::is_bypass`.
pub(crate) fn bypass(c: &Coefficients) -> bool {
    c.band_count == 0 && c.preamp_gain == 1.0
}

/// `HptfCoefficients::operator==` (IEEE comparison of every float).
pub(crate) fn same(a: &Coefficients, b: &Coefficients) -> bool {
    a.sample_rate == b.sample_rate
        && a.band_count == b.band_count
        && a.preamp_gain == b.preamp_gain
        && a.sections.iter().zip(&b.sections).all(|(x, y)| {
            x.b0 == y.b0 && x.b1 == y.b1 && x.b2 == y.b2 && x.a1 == y.a1 && x.a2 == y.a2
        })
        && a.max_response_db == b.max_response_db
        && a.auto_trim_db == b.auto_trim_db
        && a.preamp_db == b.preamp_db
}

/// `hptf_snapshot_info` and `to_hptf_info`: the left and right infos and whether they are the
/// same curve.
pub(crate) fn hptf_infos(snapshot: &Snapshot) -> (adm_hptf_info_t, adm_hptf_info_t, bool) {
    let info = |c: &Coefficients| adm_hptf_info_t {
        struct_size: size_of::<adm_hptf_info_t>() as u32,
        enabled: i32::from(!bypass(c)),
        band_count: c.band_count,
        sample_rate: c.sample_rate,
        preamp_db: c.preamp_db,
        auto_trim_db: c.auto_trim_db,
        max_response_db: c.max_response_db,
        reserved_v1_37: 0,
        applied_revision: snapshot.revision,
    };
    let right = if snapshot.independent != 0 {
        &snapshot.right
    } else {
        &snapshot.coefficients
    };
    (
        info(&snapshot.coefficients),
        info(right),
        same(right, &snapshot.coefficients),
    )
}

fn snapshot(output: &EngineSceneOutput) -> Snapshot {
    output.hptf_snapshot()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_hptf(
    output: *mut adm_scene_output_t,
    config: *const adm_hptf_config_t,
) -> i32 {
    scene_hptf(output, config.cast(), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_hptf_ex(
    output: *const adm_scene_output_t,
    config: *const adm_hptf_config_t,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { scene_hptf_ex(output, config.cast(), 0, error) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_hptf_parameters(
    output: *mut adm_scene_output_t,
    parameters: *const adm_hptf_parameters_t,
) -> i32 {
    scene_hptf(output, parameters.cast(), 1)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_hptf_stereo(
    output: *mut adm_scene_output_t,
    config: *const adm_hptf_stereo_config_t,
) -> i32 {
    scene_hptf(output, config.cast(), 2)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_hptf_stereo_ex(
    output: *const adm_scene_output_t,
    config: *const adm_hptf_stereo_config_t,
    error: *mut *mut c_char,
) -> i32 {
    unsafe { scene_hptf_ex(output, config.cast(), 2, error) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_set_hptf_stereo_parameters(
    output: *mut adm_scene_output_t,
    parameters: *const adm_hptf_stereo_parameters_t,
) -> i32 {
    scene_hptf(output, parameters.cast(), 3)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_get_hptf_info(
    output: *mut adm_scene_output_t,
    out: *mut adm_hptf_info_t,
) -> i32 {
    if !unsafe { output_valid(out) } {
        return INVALID_ARGUMENT;
    }
    output_call(output, |h| {
        let (left, _, symmetric) = hptf_infos(&snapshot(h));
        if !symmetric {
            return Err(invalid(2, "左右 HpTF 不同,请使用双耳状态查询"));
        }
        unsafe { write_sized(out, left) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_get_hptf_stereo_info(
    output: *mut adm_scene_output_t,
    out: *mut adm_hptf_stereo_info_t,
) -> i32 {
    if !unsafe { output_valid(out) } {
        return INVALID_ARGUMENT;
    }
    output_call(output, |h| {
        let (left, right, _) = hptf_infos(&snapshot(h));
        unsafe {
            write_sized(
                out,
                adm_hptf_stereo_info_t {
                    struct_size: size_of::<adm_hptf_stereo_info_t>() as u32,
                    reserved_v1_50: 0,
                    left,
                    right,
                },
            );
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_output_get_status(
    output: *mut adm_scene_output_t,
    out: *mut adm_scene_output_status_t,
) -> i32 {
    if !unsafe { output_valid(out) } {
        return INVALID_ARGUMENT;
    }
    output_call(output, |h| {
        let v = h.status();
        unsafe {
            write_sized(
                out,
                adm_scene_output_status_t {
                    struct_size: 0,
                    state: v.state as i32,
                    epoch_id: v.epoch,
                    consumed_frames: v.consumed_frames,
                    presented_frames: v.presented_frames,
                    queued_frames: v.queued_frames,
                    underruns: v.underruns,
                    clock_kind: if v.media_clock != 0 {
                        OUTPUT_CLOCK_MEDIA
                    } else {
                        0
                    },
                    recovering: i32::from(v.recovering != 0),
                    failed: i32::from(v.failed != 0),
                    ended: i32::from(v.ended != 0),
                },
            );
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn renderer(selection: i32, layout: &CStr, sofa: Option<&CStr>) -> adm_scene_renderer_config_t {
        adm_scene_renderer_config_t {
            struct_size: size_of::<adm_scene_renderer_config_t>() as u32,
            renderer: selection,
            output_layout: layout.as_ptr(),
            sofa_path: sofa.map_or(ptr::null(), CStr::as_ptr),
            speaker_geometry: 0,
            speaker_spread_mode: 0,
            binaural_spread_mode: 0,
            lfe_routing_mode: 0,
            object_smoothing_frames: 0,
        }
    }

    fn message(result: Result<Renderer, Failure>) -> (i32, String) {
        match result {
            Err(Failure::Error(e)) => (e.code, String::from_utf8(e.message).unwrap()),
            Err(Failure::Exception) => panic!("exception"),
            Ok(_) => (0, String::new()),
        }
    }

    #[test]
    fn renderer_configurations_are_checked_in_the_former_order() {
        assert_eq!(
            message(unsafe { renderer_config(ptr::null(), 48000) }),
            (
                1,
                "invalid live Scene renderer configuration struct_size".into()
            )
        );
        let mut short = renderer(RENDERER_SAF, c"5.1", None);
        short.struct_size -= 1;
        assert_eq!(message(unsafe { renderer_config(&short, 48000) }).0, 1);
        let mut value = renderer(RENDERER_SAF, c"5.1", None);
        value.object_smoothing_frames = 48001;
        assert_eq!(
            message(unsafe { renderer_config(&value, 48000) }),
            (1, "invalid live Scene renderer configuration value".into())
        );
        assert_eq!(
            message(unsafe { renderer_config(&renderer(3, c"5.1", None), 48000) }).0,
            1
        );
        assert_eq!(
            message(unsafe { renderer_config(&renderer(RENDERER_SAF, c"", None), 48000) }),
            (
                1,
                "Speaker live Scene rendering requires an output layout".into()
            )
        );
        assert_eq!(
            message(unsafe { renderer_config(&renderer(RENDERER_SAF, c"0+2+0", None), 48000) }).0,
            2
        );
        assert_eq!(
            message(unsafe {
                renderer_config(&renderer(RENDERER_SAF, c"5.1", Some(c"a.sofa")), 48000)
            }),
            (
                2,
                "a live Scene SOFA path is valid only for SAF binaural rendering".into()
            )
        );
        assert_eq!(
            message(unsafe {
                renderer_config(&renderer(RENDERER_SAF_BINAURAL, c"5.1", None), 48000)
            }),
            (
                2,
                "SAF binaural live Scene output layout must be 'binaural'".into()
            )
        );
        let mut binaural = unsafe {
            renderer_config(
                &renderer(RENDERER_SAF_BINAURAL, c"", Some(c"dir/hrtf.sofa")),
                44100,
            )
        }
        .ok()
        .unwrap();
        binaural.convert_sofa().unwrap();
        assert_eq!(binaural.layout, b"binaural");
        let config = binaural.config(true).ok().unwrap();
        assert_eq!(config.sample_rate, 44100);
        assert!(config.room.package.is_none());
        assert_eq!(
            config.sofa.map(|sofa| sofa.file_name),
            Some(&b"hrtf.sofa"[..])
        );
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct Item {
        struct_size: u32,
        value: u32,
    }

    #[test]
    fn sized_arrays_follow_the_first_struct_size() {
        assert_eq!(
            unsafe { sized::<Item>(ptr::null(), 0) }
                .ok()
                .unwrap()
                .unwrap()
                .len(),
            0
        );
        assert!(
            unsafe { sized::<Item>(ptr::null(), 1) }
                .ok()
                .unwrap()
                .is_none()
        );
        // Records 12 bytes apart: the reader honours the caller's stride.
        let mut bytes = [0u32; 6];
        bytes[0] = 12;
        bytes[1] = 7;
        bytes[3] = 12;
        bytes[4] = 9;
        let read = unsafe { sized(bytes.as_ptr().cast::<Item>(), 2) }
            .ok()
            .unwrap()
            .unwrap();
        assert_eq!((read[0].value, read[1].value), (7, 9));
        bytes[3] = 8;
        assert!(
            unsafe { sized(bytes.as_ptr().cast::<Item>(), 2) }
                .ok()
                .unwrap()
                .is_none()
        );
        let short = [4u32, 0];
        assert!(
            unsafe { sized(short.as_ptr().cast::<Item>(), 1) }
                .ok()
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn sized_outputs_keep_the_caller_size() {
        let mut out = [0xFFu8; 12];
        out[..4].copy_from_slice(&8u32.to_ne_bytes());
        unsafe {
            write_sized(
                out.as_mut_ptr().cast::<adm_scene_output_format_t>(),
                adm_scene_output_format_t {
                    struct_size: 0,
                    sample_format: 0,
                    sample_rate: 48000,
                    channels: 2,
                    interleaved: 1,
                    reserved_v1_35: 0,
                },
            )
        };
        assert_eq!(&out[..4], &8u32.to_ne_bytes());
        assert_eq!(&out[4..8], &0i32.to_ne_bytes());
        assert_eq!(&out[8..], &[0xFF; 4]);
        assert!(!unsafe { output_valid(out.as_ptr().cast::<u32>()) } || out[0] >= 4);
        let tiny = [3u32];
        assert!(!unsafe { output_valid(tiny.as_ptr()) });
    }

    fn room_status_fixture(message: &str) -> RoomStatus {
        RoomStatus {
            state: 0,
            requested_revision: 0,
            applied_revision: 0,
            transition_sample: 0,
            material_frames: 0,
            tail_frames: 0,
            buffer_frames: 0,
            material_filter_guard_frames: 0,
            constrained_sources: 0,
            message: message.to_owned(),
            quality: mradm_dsp::room::quality::Quality::Exact,
            reflection_update_frames: 0,
        }
    }

    #[test]
    fn room_documents_match_the_former_json() {
        let mut status = room_status_fixture("准备中");
        status.state = 1;
        status.quality = mradm_dsp::room::quality::Quality::Realtime;
        status.requested_revision = 3;
        status.buffer_frames = 256;
        assert_eq!(
            status_json(&status).unwrap(),
            "{\"applied_revision\":0,\"buffer_frames\":256,\"constrained_sources\":0,\
             \"material_filter_guard_frames\":0,\"material_frames\":0,\"message\":\"准备中\",\
             \"quality\":\"realtime\",\"reflection_update_frames\":0,\"requested_revision\":3,\
             \"schema\":\"mradm.room-status\",\"state\":\"preparing\",\"tail_frames\":0,\
             \"transition_sample\":0,\"version\":1}"
        );
        status.state = 5;
        assert!(status_json(&status).is_none());
        let source = SourcePosition {
            element_id: 9,
            position: [1.0, 0.5, 0.0],
            direction: [0.0, 1.0, 0.0],
            distance: 1.25,
            constrained: false,
            present: true,
        };
        assert_eq!(
            source_json(4, &source).unwrap(),
            "{\"constrained\":false,\"direction\":[0.0,1.0,0.0],\"distance_metres\":1.25,\
             \"element_id\":4,\"position_metres\":[1.0,0.5,0.0],\"present\":true,\
             \"schema\":\"mradm.room-source\",\"version\":1}"
        );
    }

    #[test]
    fn room_json_allocation_failures_return_internal_and_allow_retry() {
        fn check(build: impl Fn() -> Option<String>) {
            let expected = build().unwrap();
            let check_output = |out: *mut c_char| unsafe {
                assert_eq!(CStr::from_ptr(out).to_bytes(), expected.as_bytes());
                string::adm_free_string(out);
            };
            for successful in 0..128 {
                let mut out = ptr::dangling_mut();
                let (code, failed) =
                    crate::test_alloc::with_allocation_failure_after(successful, || {
                        guard::code(|| unsafe { room_json(&mut out, &build) })
                    });
                if !failed {
                    assert!(
                        successful > 1,
                        "must cover construction and the ABI-owned copy"
                    );
                    assert_eq!(code, OK);
                    check_output(out);
                    return;
                }
                assert_eq!(code, INTERNAL, "allocation {successful}");
                assert!(
                    out.is_null(),
                    "allocation {successful} published a partial JSON"
                );
                assert_eq!(unsafe { room_json(&mut out, &build) }, OK);
                check_output(out);
            }
            panic!("did not reach the end of JSON allocations");
        }

        let message = "\"\\\0\u{1}\u{8}\t\n\u{c}\r准备中/\u{2028}".repeat(256);
        let mut status = room_status_fixture(&message);
        status.requested_revision = u64::MAX;
        check(|| status_json(&status));

        // Scientific notation must not allocate a temporary exponent string. Both finite
        // extremes and non-finite values follow the original nlohmann formatting rules.
        let source = SourcePosition {
            element_id: 1,
            position: [-0.0, f64::from_bits(1), f64::MAX],
            direction: [f64::NAN, f64::INFINITY, f64::NEG_INFINITY],
            distance: 1e-9,
            constrained: true,
            present: true,
        };
        check(|| source_json(u64::MAX, &source));
    }

    #[test]
    fn room_json_rejects_invalid_status_without_publishing() {
        let mut status = room_status_fixture("");
        for state in [5, 6] {
            status.state = state;
            let mut out = ptr::dangling_mut();
            assert_eq!(
                unsafe { room_json(&mut out, || status_json(&status)) },
                INTERNAL
            );
            assert!(out.is_null());
        }
    }

    #[test]
    fn hptf_infos_compare_both_curves() {
        let mut s: Snapshot = unsafe { std::mem::zeroed() };
        s.coefficients.preamp_gain = 1.0;
        s.right.preamp_gain = 1.0;
        s.revision = 4;
        let (left, right, symmetric) = hptf_infos(&s);
        assert!(symmetric);
        assert_eq!((left.enabled, right.applied_revision), (0, 4));
        s.independent = 1;
        s.right.band_count = 1;
        let (_, right, symmetric) = hptf_infos(&s);
        assert!(!symmetric);
        assert_eq!((right.enabled, right.band_count), (1, 1));
    }

    #[test]
    fn null_handles_are_rejected() {
        unsafe {
            let mut status = SUBMIT_CLOSED + 1;
            assert_eq!(
                adm_scene_stream_submit_frame(ptr::null_mut(), ptr::null(), 0, &mut status),
                INVALID_ARGUMENT
            );
            assert_eq!(status, SUBMIT_CLOSED);
            assert_eq!(adm_scene_stream_log_count(ptr::null_mut()), 0);
            assert_eq!(*adm_scene_stream_last_error_message(ptr::null()), 0);
            assert_eq!(*adm_scene_output_last_error_message(ptr::null()), 0);
            assert_eq!(adm_scene_output_play(ptr::null_mut()), INVALID_ARGUMENT);
            let mut error = ptr::null_mut();
            assert_eq!(
                adm_scene_output_set_hptf_ex(ptr::null(), ptr::null(), &mut error),
                INVALID_ARGUMENT
            );
            assert_eq!(
                adm_scene_output_set_hptf_ex(ptr::null(), ptr::null(), ptr::null_mut()),
                INVALID_ARGUMENT
            );
            let mut request = 7;
            assert_eq!(
                adm_scene_stream_switch_room(ptr::null(), ptr::null(), &mut request, &mut error),
                INVALID_ARGUMENT
            );
            assert_eq!(request, 0);
            adm_destroy_scene_stream(ptr::null_mut());
            adm_destroy_scene_output(ptr::null_mut());
        }
    }

    #[test]
    fn messages_report_allocation_failure() {
        let mut m = Message::default();
        assert_eq!(unsafe { *m.as_ptr() }, 0);
        let (stored, failed) = crate::test_alloc::with_allocation_failure(|| m.set(b"error"));
        assert!(failed && !stored);
        assert!(m.set(b"error"));
        assert_eq!(unsafe { CStr::from_ptr(m.as_ptr()) }.to_bytes(), b"error");
    }
}
