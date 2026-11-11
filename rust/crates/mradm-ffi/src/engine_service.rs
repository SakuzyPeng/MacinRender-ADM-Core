//! Private boundary of the offline render service (`mradm-engine`
//! `render`): the C++ `RenderService` steps a handle, executes its island requests and answers
//! them. Texts in events and results live until the next call on the handle.
use crate::adm::Text;
use crate::engine::{EngineLog, EnginePlatform, EngineStatus, platform};
use crate::engine_render::{FfiHost, log_views};
use crate::render::RenderPrepared;
use crate::room::RoomProfileHandle;
use crate::scene::{DirectMatrix, DirectMatrixView, SceneHandle};
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::Screens;
use mradm_engine::offline::{Metrics, Progress};
use mradm_engine::render::{
    self, Event, Island, Outcome, PathText, Render, Reply, Request, Reuse, RoomOptions, SofaPath,
};
use mradm_engine::request::{self, Packager};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
};

/// A path's UTF-8 text and the caller's spelling of it in messages.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EnginePath {
    utf8: Text,
    display: Text,
}

/// `RenderRequest` (`RenderOptions`); the enum fields are the C++ values. `room_profile` may be
/// null.
#[repr(C)]
pub struct EngineRenderRequest {
    input: EnginePath,
    has_output: u32,
    output: EnginePath,
    renderer: u32,
    output_layout: Text,
    has_input_layout: u32,
    input_layout: Text,
    input_channel_labels: *const Text,
    input_channel_labels_len: usize,
    measure_loudness: u32,
    loudness_target_lufs: f32,
    peak_limit: u32,
    peak_limit_dbtp: f32,
    peak_normalize_to_limit: u32,
    final_gain_db: f64,
    output_bit_depth: u32,
    opus_bitrate_per_ch_kbps: u32,
    apac_bitrate_kbps: u32,
    apac_drc_music: u32,
    apac_caf: u32,
    has_sofa: u32,
    sofa: EnginePath,
    sofa_file_name: Text,
    has_policy_path: u32,
    policy_path: EnginePath,
    has_policy_json: u32,
    policy_json: Text,
    has_matrix_path: u32,
    matrix_path: EnginePath,
    has_matrix_json: u32,
    matrix_json: Text,
    has_report_path: u32,
    report_path: EnginePath,
    capture_report: u32,
    default_interp_ms: u32,
    object_smoothing_frames: u32,
    apple_geometry: u32,
    routing: u32,
    speaker_spread: u32,
    triple_balance_mode: u32,
    binaural_spread: u32,
    split_power: u32,
    apple_spatial_preset: u32,
    apple_speaker_rendering_flags: u32,
    listener_orientation: [f32; 3],
    render_start_sec: f64,
    has_end: u32,
    render_end_sec: f64,
    room_profile: *const RoomProfileHandle,
    room_hrir: Text,
    room_metres: f64,
    room_quality: u32,
    has_reference: u32,
    reference: RoomScreenGeometry,
    has_reproduction: u32,
    reproduction: RoomScreenGeometry,
    internal_allow_speaker_stereo: u32,
    iamf_mp4: u32,
    iamf_layers: *const Text,
    iamf_layers_len: usize,
}

/// A progress event (`ProgressEvent`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineProgress {
    stage: u32,
    operation: u32,
    fraction: f64,
    stage_fraction: f64,
    current: u64,
    total: u64,
    message: Text,
}

/// One event: `kind` 0 nothing, 1 progress, 2 log (`level`, `module`, `message`), 3 island, 4
/// done (`mradm_engine_render_result`). Islands: 1 MP4 packager probe, 2 Apple prepare, 3 Apple
/// render, 4 FLAC, 5 Opus MKA, 6 APAC, 7 IAMF, 8 MP4 packaging, 9 metadata; `src` / `dst` the
/// files, the Apple plan's output in `dst`.
#[repr(C)]
pub struct EngineRenderEvent {
    kind: u32,
    progress: EngineProgress,
    level: u32,
    module: Text,
    message: Text,
    island: u32,
    src: EnginePath,
    dst: EnginePath,
    layout: Text,
    bitrate: u32,
    drc_music: u32,
    caf: u32,
    has_lufs: u32,
    has_peak: u32,
    lufs: f64,
    peak_dbtp: f64,
    layers: *const Text,
    layers_len: usize,
    has_window: u32,
    window_start: u64,
    window_count: u64,
    has_meter_window: u32,
    meter_start: u64,
    meter_count: u64,
    has_matrix: u32,
    matrix: DirectMatrixView,
    encoder: Text,
    date_utc: Text,
    renderer: Text,
}

/// An island's answer: `code` 0 for success; the Apple render's metrics; the probed packager (0
/// none, 1 mp4box, 2 ffmpeg) and ffmpeg major version (-1 unknown).
#[repr(C)]
pub struct EngineIslandReply {
    code: i32,
    message: Text,
    context: Text,
    has_lufs: u32,
    has_peak: u32,
    lufs: f64,
    peak_dbtp: f64,
    packager: u32,
    ffmpeg_major: i32,
}

/// `RenderResult`: `code` 0 on success; one log line per result (empty module).
#[repr(C)]
pub struct EngineRenderResult {
    code: i32,
    message: Text,
    context: Text,
    has_output: u32,
    output_path: Text,
    has_metrics: u32,
    has_lufs: u32,
    has_peak: u32,
    lufs: f64,
    peak_dbtp: f64,
    logs: *const EngineLog,
    logs_len: usize,
    has_report: u32,
    report: Text,
}

/// A render in progress, and what its last event points at.
pub struct EngineRender {
    render: Render,
    island: Option<Island>,
    texts: Vec<Text>,
    matrix: Option<Box<DirectMatrix>>,
    message: String,
    logs: Vec<request::Log>,
    log_views: Vec<EngineLog>,
    result: Option<Retained>,
}

/// The result texts the result view borrows.
struct Retained {
    message: String,
    context: String,
    output_path: String,
    report: String,
}

unsafe fn bytes<'a>(t: &Text) -> Option<&'a [u8]> {
    if t.len == 0 {
        return Some(&[]);
    }
    (!t.data.is_null()).then(|| unsafe { slice::from_raw_parts(t.data, t.len) })
}

unsafe fn string(t: &Text) -> Option<String> {
    unsafe { bytes(t) }.map(|b| String::from_utf8_lossy(b).into_owned())
}

unsafe fn path(p: &EnginePath) -> Option<PathText> {
    let utf8 = std::str::from_utf8(unsafe { bytes(&p.utf8)? }).ok()?;
    if utf8.contains('\0') {
        return None;
    }
    Some(PathText {
        utf8: utf8.to_owned(),
        display: unsafe { string(&p.display)? },
    })
}

unsafe fn strings(data: *const Text, len: usize) -> Option<Vec<String>> {
    if len == 0 {
        return Some(Vec::new());
    }
    if data.is_null() {
        return None;
    }
    unsafe { slice::from_raw_parts(data, len) }
        .iter()
        .map(|t| unsafe { string(t) })
        .collect()
}

pub(crate) unsafe fn request_of(r: &EngineRenderRequest) -> Option<Request> {
    let flag = |v: u32| v != 0;
    let opt_path = |has: u32, p: &EnginePath| -> Option<Option<PathText>> {
        if has == 0 {
            Some(None)
        } else {
            unsafe { path(p) }.map(Some)
        }
    };
    let opt_bytes = |has: u32, t: &Text| -> Option<Option<Vec<u8>>> {
        if has == 0 {
            Some(None)
        } else {
            unsafe { bytes(t) }.map(|b| Some(b.to_vec()))
        }
    };
    Some(Request {
        input: unsafe { path(&r.input)? },
        output: opt_path(r.has_output, &r.output)?,
        renderer: r.renderer,
        output_layout: unsafe { string(&r.output_layout)? },
        input_layout: if r.has_input_layout != 0 {
            Some(unsafe { string(&r.input_layout)? })
        } else {
            None
        },
        input_channel_labels: unsafe {
            strings(r.input_channel_labels, r.input_channel_labels_len)?
        },
        measure_loudness: flag(r.measure_loudness),
        loudness_target_lufs: r.loudness_target_lufs,
        peak_limit: flag(r.peak_limit),
        peak_limit_dbtp: r.peak_limit_dbtp,
        peak_normalize_to_limit: flag(r.peak_normalize_to_limit),
        final_gain_db: r.final_gain_db,
        output_bit_depth: r.output_bit_depth,
        opus_bitrate_per_ch_kbps: r.opus_bitrate_per_ch_kbps,
        apac_bitrate_kbps: r.apac_bitrate_kbps,
        apac_drc_music: flag(r.apac_drc_music),
        apac_caf: flag(r.apac_caf),
        sofa: if r.has_sofa != 0 {
            Some(SofaPath {
                path: unsafe { path(&r.sofa)? },
                file_name: unsafe { bytes(&r.sofa_file_name)? }.to_vec(),
            })
        } else {
            None
        },
        semantic_policy_path: opt_path(r.has_policy_path, &r.policy_path)?,
        semantic_policy_json: opt_bytes(r.has_policy_json, &r.policy_json)?,
        matrix_path: opt_path(r.has_matrix_path, &r.matrix_path)?,
        matrix_json: opt_bytes(r.has_matrix_json, &r.matrix_json)?,
        semantic_report_path: opt_path(r.has_report_path, &r.report_path)?,
        capture_semantic_report: flag(r.capture_report),
        default_interp_ms: r.default_interp_ms,
        object_smoothing_frames: r.object_smoothing_frames,
        apple_geometry: flag(r.apple_geometry),
        routing: r.routing,
        speaker_spread: r.speaker_spread,
        triple_balance_mode: r.triple_balance_mode,
        binaural_spread: r.binaural_spread,
        split_power: flag(r.split_power),
        apple_spatial_preset: r.apple_spatial_preset,
        apple_speaker_rendering_flags: flag(r.apple_speaker_rendering_flags),
        listener_orientation: r.listener_orientation,
        render_start_sec: r.render_start_sec,
        render_end_sec: (r.has_end != 0).then_some(r.render_end_sec),
        room: RoomOptions {
            package: unsafe { crate::room::package(r.room_profile) },
            hrir: unsafe { bytes(&r.room_hrir)? }.to_vec(),
            metres: r.room_metres,
            quality: r.room_quality,
        },
        screens: Screens {
            has_reference: r.has_reference != 0,
            reference: r.reference,
            has_reproduction: r.has_reproduction != 0,
            reproduction: r.reproduction,
        },
        internal_allow_speaker_stereo: flag(r.internal_allow_speaker_stereo),
        iamf_mp4: flag(r.iamf_mp4),
        iamf_layers: unsafe { strings(r.iamf_layers, r.iamf_layers_len)? },
    })
}

/// Starts a render. `preimported` (may be null) is a preview session's policy-applied scene, with
/// a cached preparation `cached` (may be null) or the Apple preparation the caller caches
/// (`apple_cached`). Preparation reuse does not require a preimported scene. The handle copies
/// what it needs. Returns 1 for invalid arguments, including paths not encoded as UTF-8 or
/// containing NUL, and 6 for a panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_begin(
    request: *const EngineRenderRequest,
    platform_record: *const EnginePlatform,
    preimported: *const SceneHandle,
    cached: *const RenderPrepared,
    apple_cached: u32,
    output: *mut *mut EngineRender,
) -> i32 {
    let Some(out) = (unsafe { output.as_mut() }) else {
        return 1;
    };
    *out = ptr::null_mut();
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(r), Some(p)) = (unsafe { request.as_ref() }, unsafe {
            platform_record.as_ref()
        }) else {
            return 1;
        };
        let (Some(request), Some(platform)) = (unsafe { request_of(r) }, unsafe { platform(p) })
        else {
            return 1;
        };
        let reuse = Reuse {
            scene: unsafe { preimported.as_ref() }.map(|scene| scene.scene().clone()),
            prepared: unsafe { cached.as_ref() }.and_then(|c| c.prepared.clone()),
            apple_prepared: apple_cached != 0,
        };
        *out = Box::into_raw(Box::new(EngineRender {
            render: Render::new(request, platform, reuse),
            island: None,
            texts: Vec::new(),
            matrix: None,
            message: String::new(),
            logs: Vec::new(),
            log_views: Vec::new(),
            result: None,
        }));
        0
    }))
    .unwrap_or(6)
}

fn progress_record(p: &Progress) -> EngineProgress {
    EngineProgress {
        stage: p.stage,
        operation: p.operation,
        fraction: p.fraction,
        stage_fraction: p.stage_fraction,
        current: p.current,
        total: p.total,
        message: Text::new(p.message),
    }
}

fn empty_path() -> EnginePath {
    EnginePath {
        utf8: Text::new(""),
        display: Text::new(""),
    }
}

fn path_record(p: &PathText) -> EnginePath {
    EnginePath {
        utf8: Text::new(&p.utf8),
        display: Text::new(&p.display),
    }
}

fn empty_event() -> EngineRenderEvent {
    EngineRenderEvent {
        kind: 0,
        progress: progress_record(&Progress {
            stage: 0,
            operation: 0,
            fraction: 0.,
            stage_fraction: 0.,
            current: 0,
            total: 0,
            message: "",
        }),
        level: 0,
        module: Text::new(""),
        message: Text::new(""),
        island: 0,
        src: empty_path(),
        dst: empty_path(),
        layout: Text::new(""),
        bitrate: 0,
        drc_music: 0,
        caf: 0,
        has_lufs: 0,
        has_peak: 0,
        lufs: 0.,
        peak_dbtp: 0.,
        layers: ptr::null(),
        layers_len: 0,
        has_window: 0,
        window_start: 0,
        window_count: 0,
        has_meter_window: 0,
        meter_start: 0,
        meter_count: 0,
        has_matrix: 0,
        matrix: DirectMatrix::empty_view(),
        encoder: Text::new(""),
        date_utc: Text::new(""),
        renderer: Text::new(""),
    }
}

/// Fills `e` with the pending island, borrowing the handle's copy of it.
fn island_record(h: &mut EngineRender, e: &mut EngineRenderEvent) {
    let island = h.island.as_ref().expect("pending island");
    e.kind = 3;
    let apple = |e: &mut EngineRenderEvent, plan: &render::ApplePlan| {
        e.dst = path_record(&plan.output_path);
        e.layout = Text::new(&plan.output_layout);
        if let Some(w) = plan.window {
            e.has_window = 1;
            e.window_start = w.start;
            e.window_count = w.count;
        }
        if let Some(w) = plan.meter_window {
            e.has_meter_window = 1;
            e.meter_start = w.start;
            e.meter_count = w.count;
        }
    };
    match island {
        Island::ProbeMp4Packager => e.island = 1,
        Island::ApplePrepare(plan) | Island::AppleRender(plan) => {
            e.island = if matches!(island, Island::ApplePrepare(_)) {
                2
            } else {
                3
            };
            apple(e, plan);
            if let Some(m) = &plan.matrix {
                let matrix = DirectMatrix::new(m.clone());
                e.has_matrix = 1;
                e.matrix = matrix.view();
                h.matrix = Some(matrix);
            }
        }
        Island::EncodeFlac { src, dst } => {
            e.island = 4;
            e.src = path_record(src);
            e.dst = path_record(dst);
        }
        Island::EncodeOpus {
            src,
            dst,
            layout,
            bitrate,
        } => {
            e.island = 5;
            e.src = path_record(src);
            e.dst = path_record(dst);
            e.layout = Text::new(layout);
            e.bitrate = *bitrate;
        }
        Island::EncodeApac {
            src,
            dst,
            layout,
            bitrate,
            drc_music,
            caf,
        } => {
            e.island = 6;
            e.src = path_record(src);
            e.dst = path_record(dst);
            e.layout = Text::new(layout);
            e.bitrate = *bitrate;
            e.drc_music = u32::from(*drc_music);
            e.caf = u32::from(*caf);
        }
        Island::EncodeIamf {
            src,
            dst,
            layout,
            bitrate,
            lufs,
            peak_dbtp,
            layers,
        } => {
            e.island = 7;
            e.src = path_record(src);
            e.dst = path_record(dst);
            e.layout = Text::new(layout);
            e.bitrate = *bitrate;
            e.has_lufs = u32::from(lufs.is_some());
            e.lufs = lufs.unwrap_or(0.);
            e.has_peak = u32::from(peak_dbtp.is_some());
            e.peak_dbtp = peak_dbtp.unwrap_or(0.);
            h.texts = layers.iter().map(|l| Text::new(l)).collect();
            e.layers = h.texts.as_ptr();
            e.layers_len = h.texts.len();
        }
        Island::PackageMp4 { src, dst } => {
            e.island = 8;
            e.src = path_record(src);
            e.dst = path_record(dst);
        }
        Island::WriteMetadata { path, fields } => {
            e.island = 9;
            e.dst = path_record(path);
            e.encoder = Text::new(&fields.encoder);
            e.date_utc = Text::new(&fields.date_utc);
            e.renderer = Text::new(&fields.renderer);
            e.layout = Text::new(&fields.output_layout);
            e.has_lufs = u32::from(fields.lufs.is_some());
            e.lufs = fields.lufs.unwrap_or(0.);
            e.has_peak = u32::from(fields.peak_dbtp.is_some());
            e.peak_dbtp = fields.peak_dbtp.unwrap_or(0.);
        }
    }
}

/// The next event; `cancelled` is the caller's stop token now. Returns 1 for invalid arguments, 6
/// for a panic (the render is then spent).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_next(
    handle: *mut EngineRender,
    cancelled: u32,
    event: *mut EngineRenderEvent,
) -> i32 {
    let (Some(h), Some(e)) = (unsafe { handle.as_mut() }, unsafe { event.as_mut() }) else {
        return 1;
    };
    *e = empty_event();
    catch_unwind(AssertUnwindSafe(|| {
        match h.render.next(cancelled != 0, &FfiHost) {
            Event::Continue => {}
            Event::Progress(p) => {
                e.kind = 1;
                e.progress = progress_record(&p);
            }
            Event::Log(l) => {
                h.message = l.message;
                e.kind = 2;
                e.level = l.level as u32;
                e.module = Text::new(l.module);
                e.message = Text::new(&h.message);
            }
            Event::Island(island) => {
                h.island = Some(island);
                island_record(h, e);
            }
            Event::Done => e.kind = 4,
        }
        0
    }))
    .unwrap_or(6)
}

/// Answers the pending island.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_resume(
    handle: *mut EngineRender,
    reply: *const EngineIslandReply,
) -> i32 {
    let (Some(h), Some(r)) = (unsafe { handle.as_mut() }, unsafe { reply.as_ref() }) else {
        return 1;
    };
    catch_unwind(AssertUnwindSafe(|| {
        let error = if r.code != 0 {
            Some(request::Error::new(
                r.code,
                unsafe { string(&r.message) }.unwrap_or_default(),
                unsafe { string(&r.context) }.unwrap_or_default(),
            ))
        } else {
            None
        };
        h.island = None;
        h.matrix = None;
        h.render.resume(Reply {
            error,
            metrics: Metrics {
                lufs: (r.has_lufs != 0).then_some(r.lufs),
                peak_dbtp: (r.has_peak != 0).then_some(r.peak_dbtp),
            },
            packager: match r.packager {
                1 => Packager::Mp4box,
                2 => Packager::Ffmpeg,
                _ => Packager::None,
            },
            ffmpeg_major: r.ffmpeg_major,
        });
        0
    }))
    .unwrap_or(6)
}

/// An island's progress event mapped into the render's range; `out.message` borrows `event`'s
/// unless it was empty.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_map_progress(
    handle: *const EngineRender,
    event: *const EngineProgress,
    output: *mut EngineProgress,
) -> i32 {
    let (Some(h), Some(e), Some(out)) = (
        unsafe { handle.as_ref() },
        unsafe { event.as_ref() },
        unsafe { output.as_mut() },
    ) else {
        return 1;
    };
    // The range only needs to know whether the message is empty: map with a static stand-in and
    // keep the caller's text.
    let raw = Progress {
        stage: e.stage,
        operation: e.operation,
        fraction: e.fraction,
        stage_fraction: e.stage_fraction,
        current: e.current,
        total: e.total,
        message: if e.message.len == 0 { "" } else { "-" },
    };
    let mapped = h.render.map_progress(&raw);
    *out = progress_record(&mapped);
    if e.message.len != 0 {
        out.message = e.message;
    }
    0
}

/// The result after the done event; views live until the next call on the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_result(
    handle: *mut EngineRender,
    output: *mut EngineRenderResult,
) -> i32 {
    let (Some(h), Some(out)) = (unsafe { handle.as_mut() }, unsafe { output.as_mut() }) else {
        return 1;
    };
    let Some(o) = h.render.outcome().cloned() else {
        return 1;
    };
    let Outcome {
        error,
        output_path,
        metrics,
        logs,
        semantic_report,
    } = o;
    h.logs = logs
        .into_iter()
        .map(|(level, message)| request::Log {
            level,
            module: "",
            message,
        })
        .collect();
    h.log_views = log_views(&h.logs);
    let retained = h.result.insert(Retained {
        message: error.as_ref().map_or(String::new(), |e| e.message.clone()),
        context: error.as_ref().map_or(String::new(), |e| e.context.clone()),
        output_path: output_path.clone().unwrap_or_default(),
        report: semantic_report.clone().unwrap_or_default(),
    });
    *out = EngineRenderResult {
        code: error.as_ref().map_or(0, |e| e.code),
        message: Text::new(&retained.message),
        context: Text::new(&retained.context),
        has_output: u32::from(output_path.is_some()),
        output_path: Text::new(&retained.output_path),
        has_metrics: u32::from(metrics.is_some()),
        has_lufs: u32::from(metrics.is_some_and(|m| m.lufs.is_some())),
        has_peak: u32::from(metrics.is_some_and(|m| m.peak_dbtp.is_some())),
        lufs: metrics.and_then(|m| m.lufs).unwrap_or(0.),
        peak_dbtp: metrics.and_then(|m| m.peak_dbtp).unwrap_or(0.),
        logs: h.log_views.as_ptr(),
        logs_len: h.log_views.len(),
        has_report: u32::from(semantic_report.is_some()),
        report: Text::new(&retained.report),
    };
    0
}

/// The preparation the render made, for the caller's cache (null when none); destroy with
/// `mradm_render_prepared_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_take_prepared(
    handle: *mut EngineRender,
) -> *mut RenderPrepared {
    let Some(h) = (unsafe { handle.as_mut() }) else {
        return ptr::null_mut();
    };
    match h.render.take_prepared() {
        Some(p) => Box::into_raw(Box::new(RenderPrepared {
            logs: Vec::new(),
            log_views: Vec::new(),
            error: None,
            report: None,
            prepared: Some(p),
        })),
        None => ptr::null_mut(),
    }
}

/// A copy of the render's scene for the Apple island; destroy with `mradm_scene_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_scene(
    handle: *const EngineRender,
) -> *mut SceneHandle {
    unsafe { handle.as_ref() }
        .and_then(|h| h.render.scene())
        .map_or(ptr::null_mut(), |s| {
            Box::into_raw(Box::new(SceneHandle::new(s.clone())))
        })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_destroy(handle: *mut EngineRender) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(handle)) }));
    }
}

/// The render request's semantic policy applied to an imported scene (a preview session or an
/// export): the in-memory JSON preferred over the file, with the warnings as log lines. Returns
/// 0 when `*result` owns the status and views (`mradm_engine_result_destroy`), including a policy
/// error; nonzero only for a broken boundary contract or panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_apply_policy(
    scene: *mut SceneHandle,
    has_json: u32,
    json: Text,
    has_path: u32,
    path_record: EnginePath,
    input_display: Text,
    result: *mut *mut crate::engine::EngineResult,
    status: *mut EngineStatus,
) -> i32 {
    crate::engine::guarded(result, status, || {
        let Some(handle) = (unsafe { scene.as_mut() }) else {
            return 1;
        };
        let json = if has_json != 0 {
            unsafe { bytes(&json) }
        } else {
            None
        };
        let policy_path = if has_path != 0 {
            let Some(path) = (unsafe { path(&path_record) }) else {
                crate::engine::finish(
                    request::Outcome::<()> {
                        logs: Vec::new(),
                        result: Err(request::Error::new(
                            request::code::INVALID_ARGUMENT,
                            "semantic policy path must use UTF-8 without NUL",
                            "",
                        )),
                    },
                    result,
                    status,
                );
                return 0;
            };
            Some(path)
        } else {
            None
        };
        let display = unsafe { string(&input_display) }.unwrap_or_default();
        let outcome = render::apply_policy(&mut handle.scene, json, policy_path.as_ref(), &display);
        handle.refresh();
        crate::engine::finish(outcome, result, status);
        0
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn filesystem_paths_reject_invalid_utf8_and_nul() {
        for raw in [&b"out-\xff.wav"[..], &b"out\0.wav"[..]] {
            let p = EnginePath {
                utf8: Text::bytes(raw),
                display: Text::bytes(raw),
            };
            assert!(unsafe { path(&p) }.is_none());
        }
        let raw = "out-\u{fffd}.wav";
        let p = EnginePath {
            utf8: Text::new(raw),
            display: Text::new(raw),
        };
        assert_eq!(unsafe { path(&p) }.unwrap().utf8, raw);
    }
}
