//! `adm_render_options_t` and its setters, with the C++ `RenderOptions` defaults and checks, and the
//! engine request record (`EngineRenderRequest`) a render or preview builds from them.
//!
//! Paths are kept as the UTF-8 the C++ `std::filesystem::path` produced when a setter stored them
//! (on Windows from the code page, `error::utf8_path`), with the SOFA file name the record needs.
use crate::cancel::adm_cancel_token_t;
use crate::error::{Failure, c_str, concat, try_box, utf8_path};
use crate::guard;
use crate::room::{self, adm_room_config_t};
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::Screens as EngineScreens;
use mradm_engine::render::{PathText, Request as EngineRequest, RoomOptions, SofaPath};
use mradm_ffi::room::RoomProfileHandle;
use std::ffi::c_char;
use std::ptr;

/// A stored path: its UTF-8 text.
pub(crate) struct Path {
    pub(crate) utf8: Vec<u8>,
}

/// The room of the options (`RoomConfig`): a share of the package, the HRIR path, the scale.
pub(crate) struct Room {
    profile: *mut RoomProfileHandle,
    hrir: Vec<u8>,
    metres: f64,
}

/// Releases a room profile handle (from `RoomProfileHandle::load` or `share`).
pub(crate) unsafe fn release_profile(profile: *mut RoomProfileHandle) {
    if !profile.is_null() {
        drop(unsafe { Box::from_raw(profile) });
    }
}

impl Drop for Room {
    fn drop(&mut self) {
        unsafe { release_profile(self.profile) };
    }
}

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_render_options_t {
    pub(crate) renderer: u32,
    output_layout: Vec<u8>,
    input_layout: Option<Vec<u8>>,
    input_channel_labels: Vec<Vec<u8>>,
    pub(crate) monitor_system_spatial: bool,
    measure_loudness: bool,
    loudness_target_lufs: f32,
    peak_limit: bool,
    peak_limit_dbtp: f32,
    peak_normalize_to_limit: bool,
    final_gain_db: f64,
    output_bit_depth: u32,
    opus_bitrate_per_ch_kbps: u32,
    apac_bitrate_kbps: u32,
    apac_drc_music: bool,
    apac_caf: bool,
    sofa: Option<Path>,
    sofa_file_name: Vec<u8>,
    pub(crate) semantic_policy_path: Option<Path>,
    pub(crate) semantic_policy_json: Option<Vec<u8>>,
    direct_speakers_matrix_path: Option<Path>,
    direct_speakers_matrix_json: Option<Vec<u8>>,
    semantic_report_path: Option<Path>,
    capture_semantic_report: bool,
    default_interp_ms: u32,
    object_smoothing_frames: u32,
    speaker_geometry: u32,
    direct_speakers_routing_mode: u32,
    speaker_spread_mode: u32,
    triple_balance_mode: u32,
    binaural_spread_mode: u32,
    lfe_routing_mode: u32,
    apple_speaker_rendering_flags: bool,
    pub(crate) render_start_sec: f64,
    pub(crate) render_end_sec: Option<f64>,
    iamf_mp4: bool,
    iamf_layers: Vec<Vec<u8>>,
    /// Borrowed; its current flag is taken when a render starts.
    pub(crate) cancel_token: *mut adm_cancel_token_t,
    room: Option<Room>,
    room_quality: u32,
    screen_reference: Option<RoomScreenGeometry>,
    screen_reproduction: Option<RoomScreenGeometry>,
}

impl adm_render_options_t {
    /// `RenderOptions{}`; `None` when out of memory.
    pub(crate) fn defaults() -> Option<Self> {
        Some(Self {
            renderer: 0,
            output_layout: concat(&[b"binaural"]).ok()?,
            input_layout: None,
            input_channel_labels: Vec::new(),
            monitor_system_spatial: false,
            measure_loudness: false,
            loudness_target_lufs: -23.0,
            peak_limit: true,
            peak_limit_dbtp: -1.0,
            peak_normalize_to_limit: false,
            final_gain_db: 0.0,
            output_bit_depth: 0,
            opus_bitrate_per_ch_kbps: 0,
            apac_bitrate_kbps: 0,
            apac_drc_music: true,
            apac_caf: false,
            sofa: None,
            sofa_file_name: Vec::new(),
            semantic_policy_path: None,
            semantic_policy_json: None,
            direct_speakers_matrix_path: None,
            direct_speakers_matrix_json: None,
            semantic_report_path: None,
            capture_semantic_report: false,
            default_interp_ms: 5,
            object_smoothing_frames: 0,
            speaker_geometry: 0,
            direct_speakers_routing_mode: 0,
            speaker_spread_mode: 0,
            triple_balance_mode: 0,
            binaural_spread_mode: 0,
            lfe_routing_mode: 0,
            apple_speaker_rendering_flags: false,
            render_start_sec: 0.0,
            render_end_sec: None,
            iamf_mp4: false,
            iamf_layers: Vec::new(),
            cancel_token: ptr::null_mut(),
            room: None,
            room_quality: 0,
            screen_reference: None,
            screen_reproduction: None,
        })
    }

    /// A copy for a preview session (the room's package shared again); `None` when out of memory.
    pub(crate) fn duplicate(&self) -> Option<Self> {
        let copy = |v: &[u8]| concat(&[v]).ok();
        let copies = |list: &[Vec<u8>]| -> Option<Vec<Vec<u8>>> {
            let mut out = Vec::new();
            out.try_reserve_exact(list.len()).ok()?;
            for item in list {
                out.push(copy(item)?);
            }
            Some(out)
        };
        let path = |p: &Option<Path>| -> Option<Option<Path>> {
            match p {
                None => Some(None),
                Some(p) => Some(Some(Path {
                    utf8: copy(&p.utf8)?,
                })),
            }
        };
        let text = |t: &Option<Vec<u8>>| -> Option<Option<Vec<u8>>> {
            match t {
                None => Some(None),
                Some(t) => Some(Some(copy(t)?)),
            }
        };
        let room = match &self.room {
            None => None,
            Some(r) => {
                let profile = unsafe { &*r.profile }.share()?;
                let hrir = match copy(&r.hrir) {
                    Some(hrir) => hrir,
                    None => {
                        unsafe { release_profile(profile) };
                        return None;
                    }
                };
                Some(Room {
                    profile,
                    hrir,
                    metres: r.metres,
                })
            }
        };
        Some(Self {
            output_layout: copy(&self.output_layout)?,
            input_layout: text(&self.input_layout)?,
            input_channel_labels: copies(&self.input_channel_labels)?,
            sofa: path(&self.sofa)?,
            sofa_file_name: copy(&self.sofa_file_name)?,
            semantic_policy_path: path(&self.semantic_policy_path)?,
            semantic_policy_json: text(&self.semantic_policy_json)?,
            direct_speakers_matrix_path: path(&self.direct_speakers_matrix_path)?,
            direct_speakers_matrix_json: text(&self.direct_speakers_matrix_json)?,
            semantic_report_path: path(&self.semantic_report_path)?,
            iamf_layers: copies(&self.iamf_layers)?,
            room,
            renderer: self.renderer,
            monitor_system_spatial: self.monitor_system_spatial,
            measure_loudness: self.measure_loudness,
            loudness_target_lufs: self.loudness_target_lufs,
            peak_limit: self.peak_limit,
            peak_limit_dbtp: self.peak_limit_dbtp,
            peak_normalize_to_limit: self.peak_normalize_to_limit,
            final_gain_db: self.final_gain_db,
            output_bit_depth: self.output_bit_depth,
            opus_bitrate_per_ch_kbps: self.opus_bitrate_per_ch_kbps,
            apac_bitrate_kbps: self.apac_bitrate_kbps,
            apac_drc_music: self.apac_drc_music,
            apac_caf: self.apac_caf,
            capture_semantic_report: self.capture_semantic_report,
            default_interp_ms: self.default_interp_ms,
            object_smoothing_frames: self.object_smoothing_frames,
            speaker_geometry: self.speaker_geometry,
            direct_speakers_routing_mode: self.direct_speakers_routing_mode,
            speaker_spread_mode: self.speaker_spread_mode,
            triple_balance_mode: self.triple_balance_mode,
            binaural_spread_mode: self.binaural_spread_mode,
            lfe_routing_mode: self.lfe_routing_mode,
            apple_speaker_rendering_flags: self.apple_speaker_rendering_flags,
            render_start_sec: self.render_start_sec,
            render_end_sec: self.render_end_sec,
            iamf_mp4: self.iamf_mp4,
            cancel_token: self.cancel_token,
            room_quality: self.room_quality,
            screen_reference: self.screen_reference,
            screen_reproduction: self.screen_reproduction,
        })
    }

    pub(crate) fn input_layout(&self) -> Option<&[u8]> {
        self.input_layout.as_deref()
    }

    pub(crate) fn input_channel_labels(&self) -> &[Vec<u8>] {
        &self.input_channel_labels
    }
}

/// A path as the engine request holds it; `None` when it is not UTF-8 or contains NUL.
pub(crate) fn path_text(utf8: &[u8]) -> Option<PathText> {
    let text = std::str::from_utf8(utf8).ok()?;
    if text.contains('\0') {
        return None;
    }
    Some(PathText {
        utf8: text.to_owned(),
        display: text.to_owned(),
    })
}

/// Text the engine matches as ASCII; other bytes become replacement characters.
pub(crate) fn lossy(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

impl adm_render_options_t {
    /// The engine request of a render of `input` (and `output`) with these options; `None` when a
    /// path is not UTF-8 or contains NUL.
    pub(crate) fn engine_request(
        &self,
        input: &[u8],
        output: Option<&[u8]>,
    ) -> Result<Option<EngineRequest>, Failure> {
        let opt_path = |p: &Option<Path>| -> Option<Option<PathText>> {
            match p {
                None => Some(None),
                Some(p) => path_text(&p.utf8).map(Some),
            }
        };
        let request = (|| {
            Some(EngineRequest {
                input: path_text(input)?,
                output: match output {
                    None => None,
                    Some(o) => Some(path_text(o)?),
                },
                renderer: self.renderer,
                output_layout: lossy(&self.output_layout),
                input_layout: self.input_layout.as_deref().map(lossy),
                input_channel_labels: self.input_channel_labels.iter().map(|l| lossy(l)).collect(),
                measure_loudness: self.measure_loudness,
                loudness_target_lufs: self.loudness_target_lufs,
                peak_limit: self.peak_limit,
                peak_limit_dbtp: self.peak_limit_dbtp,
                peak_normalize_to_limit: self.peak_normalize_to_limit,
                final_gain_db: self.final_gain_db,
                output_bit_depth: self.output_bit_depth,
                opus_bitrate_per_ch_kbps: self.opus_bitrate_per_ch_kbps,
                apac_bitrate_kbps: self.apac_bitrate_kbps,
                apac_drc_music: self.apac_drc_music,
                apac_caf: self.apac_caf,
                sofa: match &self.sofa {
                    None => None,
                    Some(p) => Some(SofaPath {
                        path: path_text(&p.utf8)?,
                        file_name: self.sofa_file_name.clone(),
                    }),
                },
                semantic_policy_path: opt_path(&self.semantic_policy_path)?,
                semantic_policy_json: self.semantic_policy_json.clone(),
                matrix_path: opt_path(&self.direct_speakers_matrix_path)?,
                matrix_json: self.direct_speakers_matrix_json.clone(),
                semantic_report_path: opt_path(&self.semantic_report_path)?,
                capture_semantic_report: self.capture_semantic_report,
                default_interp_ms: self.default_interp_ms,
                object_smoothing_frames: self.object_smoothing_frames,
                apple_geometry: self.speaker_geometry == 1,
                routing: self.direct_speakers_routing_mode,
                speaker_spread: self.speaker_spread_mode,
                triple_balance_mode: self.triple_balance_mode,
                binaural_spread: self.binaural_spread_mode,
                split_power: self.lfe_routing_mode == 1,
                apple_spatial_preset: 0,
                apple_speaker_rendering_flags: self.apple_speaker_rendering_flags,
                listener_orientation: [0.0; 3],
                render_start_sec: self.render_start_sec,
                render_end_sec: self.render_end_sec,
                room: self.room.as_ref().map_or_else(
                    || RoomOptions {
                        quality: self.room_quality,
                        ..RoomOptions::default()
                    },
                    |r| RoomOptions {
                        package: unsafe { r.profile.as_ref() }.map(RoomProfileHandle::package),
                        hrir: r.hrir.clone(),
                        metres: r.metres,
                        quality: self.room_quality,
                    },
                ),
                screens: EngineScreens {
                    has_reference: self.screen_reference.is_some(),
                    reference: self.screen_reference.unwrap_or_default(),
                    has_reproduction: self.screen_reproduction.is_some(),
                    reproduction: self.screen_reproduction.unwrap_or_default(),
                },
                internal_allow_speaker_stereo: false,
                iamf_mp4: self.iamf_mp4,
                iamf_layers: self.iamf_layers.iter().map(|l| lossy(l)).collect(),
            })
        })();
        Ok(request)
    }
}

/// `split_csv_list`: comma-separated items with surrounding whitespace trimmed; empty items kept
/// only when `preserve_empty`.
fn split_csv(csv: &[u8], preserve_empty: bool) -> Result<Vec<Vec<u8>>, Failure> {
    let blank = |b: &u8| matches!(b, b' ' | b'\t' | b'\r' | b'\n');
    let mut out = Vec::new();
    for item in csv.split(|&b| b == b',') {
        let trimmed = match (
            item.iter().position(|b| !blank(b)),
            item.iter().rposition(|b| !blank(b)),
        ) {
            (Some(first), Some(last)) => &item[first..=last],
            _ if preserve_empty => &[][..],
            _ => continue,
        };
        out.try_reserve(1).map_err(|_| Failure::Exception)?;
        out.push(concat(&[trimmed])?);
    }
    Ok(out)
}

/// `path.filename()` of a UTF-8 path, by the rules of the platform's `std::filesystem`.
pub(crate) fn file_name(path: &[u8]) -> &[u8] {
    let separator = |b: u8| b == b'/' || (cfg!(windows) && b == b'\\');
    let mut start = 0;
    if cfg!(windows) {
        if path.len() >= 2 && path[1] == b':' && path[0].is_ascii_alphabetic() {
            start = 2;
        } else if path.len() > 2 && separator(path[0]) && separator(path[1]) && !separator(path[2])
        {
            start = path[2..]
                .iter()
                .position(|&b| separator(b))
                .map_or(path.len(), |p| p + 2);
        }
    }
    let rest = &path[start..];
    match rest.iter().rposition(|&b| separator(b)) {
        Some(last) => &rest[last + 1..],
        None => rest,
    }
}

/// A C string as an optional stored value: NULL or empty clears it.
unsafe fn optional(value: *const c_char) -> Option<&'static [u8]> {
    if value.is_null() || unsafe { *value } == 0 {
        None
    } else {
        Some(unsafe { c_str(value) })
    }
}

/// A path setter: NULL or empty clears; a path the C++ could not convert is INTERNAL.
unsafe fn set_path(slot: &mut Option<Path>, value: *const c_char) -> Result<(), Failure> {
    *slot = match unsafe { optional(value) } {
        None => None,
        Some(native) => Some(Path {
            utf8: utf8_path(native).ok_or(Failure::Exception)?,
        }),
    };
    Ok(())
}

fn done(result: Result<(), Failure>) -> i32 {
    match result {
        Ok(()) => guard::OK,
        Err(_) => guard::INTERNAL,
    }
}

/// The options of a setter, or the setter's answer for NULL options.
macro_rules! options_or {
    ($opts:expr, $null:expr) => {
        match unsafe { $opts.as_mut() } {
            Some(o) => o,
            None => return $null,
        }
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn adm_create_render_options() -> *mut adm_render_options_t {
    guard::pointer(|| adm_render_options_t::defaults().map_or(ptr::null_mut(), try_box))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_render_options(opts: *mut adm_render_options_t) {
    if !opts.is_null() {
        drop(unsafe { Box::from_raw(opts) });
    }
}

/// An enum setter: values in `0..=max` are stored, others are INVALID; NULL options are OK.
unsafe fn set_enum(
    opts: *mut adm_render_options_t,
    value: i32,
    max: i32,
    set: impl FnOnce(&mut adm_render_options_t, u32),
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if !(0..=max).contains(&value) {
        return guard::INVALID_ARGUMENT;
    }
    set(o, value as u32);
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_renderer(
    opts: *mut adm_render_options_t,
    renderer: i32,
) -> i32 {
    unsafe { set_enum(opts, renderer, 8, |o, v| o.renderer = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_output_layout(
    opts: *mut adm_render_options_t,
    layout: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if layout.is_null() {
        return guard::INVALID_ARGUMENT;
    }
    done(concat(&[unsafe { c_str(layout) }]).map(|l| o.output_layout = l))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_input_layout(
    opts: *mut adm_render_options_t,
    layout: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    let value = match unsafe { optional(layout) } {
        None | Some(b"auto") => None,
        Some(l) => match concat(&[l]) {
            Ok(l) => Some(l),
            Err(_) => return guard::INTERNAL,
        },
    };
    o.input_layout = value;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_input_channel_labels(
    opts: *mut adm_render_options_t,
    labels_csv: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    done(match unsafe { optional(labels_csv) } {
        None => {
            o.input_channel_labels.clear();
            Ok(())
        }
        Some(csv) => split_csv(csv, true).map(|labels| o.input_channel_labels = labels),
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_output_bit_depth(
    opts: *mut adm_render_options_t,
    depth: i32,
) -> i32 {
    unsafe { set_enum(opts, depth, 2, |o, v| o.output_bit_depth = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_loudness_target(
    opts: *mut adm_render_options_t,
    lufs: f64,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if !lufs.is_finite() || !(-70.0..=0.0).contains(&lufs) {
        return guard::INVALID_ARGUMENT;
    }
    o.loudness_target_lufs = lufs as f32;
    o.measure_loudness = true;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_peak_limit(
    opts: *mut adm_render_options_t,
    enabled: i32,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.peak_limit = enabled != 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_monitor_system_spatial(
    opts: *mut adm_render_options_t,
    enabled: i32,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.monitor_system_spatial = enabled != 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_peak_limit_dbtp(
    opts: *mut adm_render_options_t,
    dbtp: f64,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if !dbtp.is_finite() || !(-60.0..=0.0).contains(&dbtp) {
        return guard::INVALID_ARGUMENT;
    }
    o.peak_limit_dbtp = dbtp as f32;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_peak_normalize_to_limit(
    opts: *mut adm_render_options_t,
    enabled: i32,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.peak_normalize_to_limit = enabled != 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_opus_bitrate_per_ch_kbps(
    opts: *mut adm_render_options_t,
    kbps: u32,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if kbps != 0 && !(6..=320).contains(&kbps) {
        return guard::INVALID_ARGUMENT;
    }
    o.opus_bitrate_per_ch_kbps = kbps;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_apac_bitrate_kbps(
    opts: *mut adm_render_options_t,
    kbps: u32,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if kbps != 0 && !(64..=32768).contains(&kbps) {
        return guard::INVALID_ARGUMENT;
    }
    o.apac_bitrate_kbps = kbps;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_apac_drc_music(
    opts: *mut adm_render_options_t,
    enabled: i32,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.apac_drc_music = enabled != 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_apple_speaker_rendering_flags(
    opts: *mut adm_render_options_t,
    enabled: i32,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.apple_speaker_rendering_flags = enabled != 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_apac_container(
    opts: *mut adm_render_options_t,
    container: i32,
) -> i32 {
    unsafe { set_enum(opts, container, 1, |o, v| o.apac_caf = v == 1) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_sofa_path(
    opts: *mut adm_render_options_t,
    sofa_path: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    done((|| {
        let mut sofa = None;
        unsafe { set_path(&mut sofa, sofa_path) }?;
        let name = concat(&[sofa.as_ref().map_or(&[][..], |p: &Path| file_name(&p.utf8))])?;
        o.sofa = sofa;
        o.sofa_file_name = name;
        Ok(())
    })())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_semantic_policy_path(
    opts: *mut adm_render_options_t,
    path: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    done(unsafe { set_path(&mut o.semantic_policy_path, path) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_semantic_report_path(
    opts: *mut adm_render_options_t,
    path: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    done(unsafe { set_path(&mut o.semantic_report_path, path) })
}

/// A text setter: NULL or empty clears.
unsafe fn set_text(slot: &mut Option<Vec<u8>>, value: *const c_char) -> i32 {
    *slot = match unsafe { optional(value) } {
        None => None,
        Some(v) => match concat(&[v]) {
            Ok(v) => Some(v),
            Err(_) => return guard::INTERNAL,
        },
    };
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_semantic_policy_json(
    opts: *mut adm_render_options_t,
    json: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    unsafe { set_text(&mut o.semantic_policy_json, json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_capture_semantic_report(
    opts: *mut adm_render_options_t,
    enabled: i32,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.capture_semantic_report = enabled != 0;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_default_interp_ms(
    opts: *mut adm_render_options_t,
    ms: u32,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if ms > 500 {
        return guard::INVALID_ARGUMENT;
    }
    o.default_interp_ms = ms;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_object_smoothing_frames(
    opts: *mut adm_render_options_t,
    frames: u32,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if frames > 48000 {
        return guard::INVALID_ARGUMENT;
    }
    o.object_smoothing_frames = frames;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_speaker_spread_mode(
    opts: *mut adm_render_options_t,
    mode: i32,
) -> i32 {
    unsafe { set_enum(opts, mode, 2, |o, v| o.speaker_spread_mode = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_triple_balance_mode(
    opts: *mut adm_render_options_t,
    mode: i32,
) -> i32 {
    unsafe { set_enum(opts, mode, 1, |o, v| o.triple_balance_mode = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_binaural_spread_mode(
    opts: *mut adm_render_options_t,
    mode: i32,
) -> i32 {
    unsafe { set_enum(opts, mode, 3, |o, v| o.binaural_spread_mode = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_speaker_geometry(
    opts: *mut adm_render_options_t,
    geometry: i32,
) -> i32 {
    unsafe { set_enum(opts, geometry, 1, |o, v| o.speaker_geometry = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_direct_speakers_routing_mode(
    opts: *mut adm_render_options_t,
    mode: i32,
) -> i32 {
    unsafe { set_enum(opts, mode, 3, |o, v| o.direct_speakers_routing_mode = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_direct_speakers_matrix_path(
    opts: *mut adm_render_options_t,
    path: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    done(unsafe { set_path(&mut o.direct_speakers_matrix_path, path) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_direct_speakers_matrix_json(
    opts: *mut adm_render_options_t,
    json: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    unsafe { set_text(&mut o.direct_speakers_matrix_json, json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_lfe_routing_mode(
    opts: *mut adm_render_options_t,
    mode: i32,
) -> i32 {
    unsafe { set_enum(opts, mode, 1, |o, v| o.lfe_routing_mode = v) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_iamf_container(
    opts: *mut adm_render_options_t,
    container: i32,
) -> i32 {
    unsafe { set_enum(opts, container, 1, |o, v| o.iamf_mp4 = v == 1) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_iamf_layers(
    opts: *mut adm_render_options_t,
    iamf_layers_csv: *const c_char,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if iamf_layers_csv.is_null() {
        o.iamf_layers = Vec::new();
        return guard::OK;
    }
    done(split_csv(unsafe { c_str(iamf_layers_csv) }, false).map(|layers| o.iamf_layers = layers))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_render_start_sec(
    opts: *mut adm_render_options_t,
    sec: f64,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if !sec.is_finite() || sec < 0.0 {
        return guard::INVALID_ARGUMENT;
    }
    o.render_start_sec = sec;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_render_end_sec(
    opts: *mut adm_render_options_t,
    sec: f64,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if !sec.is_finite() {
        return guard::INVALID_ARGUMENT;
    }
    // sec <= 0 clears the end (render to the end); a positive end is checked at render time.
    o.render_end_sec = (sec > 0.0).then_some(sec);
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_final_gain_db(
    opts: *mut adm_render_options_t,
    db: f64,
) -> i32 {
    let o = options_or!(opts, guard::OK);
    if !db.is_finite() {
        return guard::INVALID_ARGUMENT;
    }
    o.final_gain_db = db;
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_cancel_token(
    opts: *mut adm_render_options_t,
    token: *mut adm_cancel_token_t,
) {
    if let Some(o) = unsafe { opts.as_mut() } {
        o.cancel_token = token;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_room(
    options: *mut adm_render_options_t,
    config: *const adm_room_config_t,
) -> i32 {
    let o = options_or!(options, guard::INVALID_ARGUMENT);
    guard::code(|| {
        if config.is_null() {
            o.room = None;
            return guard::OK;
        }
        let converted = match unsafe { room::config(config) } {
            Ok(converted) => converted,
            Err(failure) => return room::failure_code(failure),
        };
        let Some(profile) = (unsafe { &*converted.profile }).share() else {
            return guard::INTERNAL;
        };
        let hrir = match concat(&[converted.hrir]) {
            Ok(hrir) => hrir,
            Err(_) => {
                unsafe { release_profile(profile) };
                return guard::INTERNAL;
            }
        };
        o.room = Some(Room {
            profile,
            hrir,
            metres: converted.metres,
        });
        guard::OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_room_quality(
    options: *mut adm_render_options_t,
    quality: i32,
) -> i32 {
    let o = options_or!(options, guard::INVALID_ARGUMENT);
    if !(0..=1).contains(&quality) {
        return guard::INVALID_ARGUMENT;
    }
    o.room_quality = quality as u32;
    guard::OK
}

/// `parse_screen_config_json`'s 1 MiB limit.
const MAX_SCREEN_JSON: usize = 1024 * 1024;

fn normalized(screen: RoomScreenGeometry) -> RoomScreenGeometry {
    RoomScreenGeometry {
        cartesian: u32::from(screen.cartesian != 0),
        reserved: 0,
        ..screen
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_options_set_screen_config_json(
    options: *mut adm_render_options_t,
    json: *const c_char,
) -> i32 {
    let o = options_or!(options, guard::INVALID_ARGUMENT);
    guard::code(|| {
        if json.is_null() {
            o.screen_reference = None;
            o.screen_reproduction = None;
            return guard::OK;
        }
        let source = unsafe { c_str(json) };
        if source.len() > MAX_SCREEN_JSON {
            return guard::INVALID_ARGUMENT;
        }
        let screens = match mradm_scene::screen::parse(source) {
            Ok(screens) => screens,
            Err(error) => return crate::error::map(error.code),
        };
        let reference = screens.reference.map(normalized);
        let reproduction = screens.reproduction.map(normalized);
        let code = room::validate_screens(reference, reproduction);
        if code != 0 {
            return crate::error::map(code);
        }
        o.screen_reference = reference;
        o.screen_reproduction = reproduction;
        guard::OK
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn split(csv: &str, keep: bool) -> Vec<String> {
        split_csv(csv.as_bytes(), keep)
            .unwrap()
            .into_iter()
            .map(|s| String::from_utf8(s).unwrap())
            .collect()
    }

    #[test]
    fn csv_lists_split_as_the_cpp_did() {
        assert_eq!(split(" L , R ,C", false), ["L", "R", "C"]);
        assert_eq!(split("L,,R", false), ["L", "R"]);
        assert_eq!(split("L,,R", true), ["L", "", "R"]);
        assert_eq!(split(",", true), ["", ""]);
        assert_eq!(split("", false), Vec::<String>::new());
        assert_eq!(split(" \t", true), [""]);
        assert_eq!(split("a\r\n", false), ["a"]);
    }

    #[test]
    fn file_names_follow_std_filesystem() {
        assert_eq!(file_name(b"dir/hrtf.sofa"), b"hrtf.sofa");
        assert_eq!(file_name(b"hrtf.sofa"), b"hrtf.sofa");
        assert_eq!(file_name(b"dir/"), b"");
        assert_eq!(file_name(b"/"), b"");
        if cfg!(windows) {
            assert_eq!(file_name(b"C:hrtf.sofa"), b"hrtf.sofa");
            assert_eq!(file_name(b"C:\\a\\hrtf.sofa"), b"hrtf.sofa");
            assert_eq!(file_name(b"\\\\server"), b"");
            assert_eq!(file_name(b"\\\\server\\share\\x.sofa"), b"x.sofa");
        } else {
            assert_eq!(file_name(b"a\\b.sofa"), b"a\\b.sofa");
        }
    }

    #[test]
    fn setters_check_as_the_cpp_did() {
        let o = adm_create_render_options();
        unsafe {
            assert_eq!(adm_render_options_set_renderer(o, 8), 0);
            assert_eq!(adm_render_options_set_renderer(o, 9), 1);
            assert_eq!(adm_render_options_set_renderer(o, -1), 1);
            assert_eq!(adm_render_options_set_renderer(ptr::null_mut(), 99), 0);
            assert_eq!(adm_render_options_set_loudness_target(o, -70.0), 0);
            assert_eq!(adm_render_options_set_loudness_target(o, 0.5), 1);
            assert_eq!(adm_render_options_set_loudness_target(o, f64::NAN), 1);
            assert_eq!(adm_render_options_set_opus_bitrate_per_ch_kbps(o, 5), 1);
            assert_eq!(adm_render_options_set_opus_bitrate_per_ch_kbps(o, 0), 0);
            assert_eq!(adm_render_options_set_output_layout(o, ptr::null()), 1);
            assert_eq!(adm_render_options_set_room_quality(ptr::null_mut(), 0), 1);
            assert_eq!(adm_render_options_set_room_quality(o, 2), 1);
            assert_eq!(adm_render_options_set_room(ptr::null_mut(), ptr::null()), 1);
            assert_eq!(adm_render_options_set_room(o, ptr::null()), 0);
            assert_eq!(adm_render_options_set_screen_config_json(o, ptr::null()), 0);
            assert_eq!(adm_render_options_set_render_end_sec(o, -1.0), 0);
            assert!((*o).render_end_sec.is_none());
            assert_eq!(adm_render_options_set_render_end_sec(o, 2.0), 0);
            assert_eq!((*o).render_end_sec, Some(2.0));
            assert_eq!(adm_render_options_set_input_layout(o, c"auto".as_ptr()), 0);
            assert!((*o).input_layout.is_none());
            adm_destroy_render_options(o);
        }
    }

    #[test]
    fn options_creation_reports_allocation_failure() {
        let (o, failed) =
            crate::test_alloc::with_allocation_failure(|| adm_create_render_options());
        assert!(failed);
        assert!(o.is_null());
    }
}
