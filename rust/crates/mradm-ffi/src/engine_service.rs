//! Private boundary of the offline render service (`mradm-engine`
//! `render`): the C++ `RenderService` steps a handle; the islands a registered runner accepts run
//! inside the step, the others are returned and answered. Texts in events and results live until
//! the next call on the handle.
use crate::adm::Text;
use crate::engine::{EngineLog, EngineStatus, Facts, PlatformIslands, platform};
use crate::engine_render::{FfiHost, log_views};
use crate::render::RenderPrepared;
use crate::render::apple_island::{Apple, AppleIslands, Preparation, Settings};
use crate::room::RoomProfileHandle;
use crate::scene::{DirectMatrix, DirectMatrixView, SceneHandle};
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::Screens;
use mradm_engine::offline::{Metrics, Progress};
use mradm_engine::render::{
    self, Event, Island, Outcome, PathText, Render, Reply, Request, Reuse, RoomOptions, SofaPath,
};
use mradm_engine::request::{self, Packager};
use mradm_scene::RenderScene;
use std::{
    cell::{Cell, RefCell},
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::Arc,
};

/// A path's UTF-8 text and the caller's spelling of it in messages.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EnginePath {
    pub utf8: Text,
    pub display: Text,
}

/// `RenderRequest` (`RenderOptions`); the enum fields are the C++ values. `room_profile` may be
/// null.
#[repr(C)]
pub struct EngineRenderRequest {
    pub input: EnginePath,
    pub has_output: u32,
    pub output: EnginePath,
    pub renderer: u32,
    pub output_layout: Text,
    pub has_input_layout: u32,
    pub input_layout: Text,
    pub input_channel_labels: *const Text,
    pub input_channel_labels_len: usize,
    pub measure_loudness: u32,
    pub loudness_target_lufs: f32,
    pub peak_limit: u32,
    pub peak_limit_dbtp: f32,
    pub peak_normalize_to_limit: u32,
    pub final_gain_db: f64,
    pub output_bit_depth: u32,
    pub opus_bitrate_per_ch_kbps: u32,
    pub apac_bitrate_kbps: u32,
    pub apac_drc_music: u32,
    pub apac_caf: u32,
    pub has_sofa: u32,
    pub sofa: EnginePath,
    pub sofa_file_name: Text,
    pub has_policy_path: u32,
    pub policy_path: EnginePath,
    pub has_policy_json: u32,
    pub policy_json: Text,
    pub has_matrix_path: u32,
    pub matrix_path: EnginePath,
    pub has_matrix_json: u32,
    pub matrix_json: Text,
    pub has_report_path: u32,
    pub report_path: EnginePath,
    pub capture_report: u32,
    pub default_interp_ms: u32,
    pub object_smoothing_frames: u32,
    pub apple_geometry: u32,
    pub routing: u32,
    pub speaker_spread: u32,
    pub triple_balance_mode: u32,
    pub binaural_spread: u32,
    pub split_power: u32,
    pub apple_spatial_preset: u32,
    pub apple_speaker_rendering_flags: u32,
    pub listener_orientation: [f32; 3],
    pub render_start_sec: f64,
    pub has_end: u32,
    pub render_end_sec: f64,
    pub room_profile: *const RoomProfileHandle,
    pub room_hrir: Text,
    pub room_metres: f64,
    pub room_quality: u32,
    pub has_reference: u32,
    pub reference: RoomScreenGeometry,
    pub has_reproduction: u32,
    pub reproduction: RoomScreenGeometry,
    pub internal_allow_speaker_stereo: u32,
    pub iamf_mp4: u32,
    pub iamf_layers: *const Text,
    pub iamf_layers_len: usize,
}

/// A progress event (`ProgressEvent`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineProgress {
    pub stage: u32,
    pub operation: u32,
    pub fraction: f64,
    pub stage_fraction: f64,
    pub current: u64,
    pub total: u64,
    pub message: Text,
}

/// One event: `kind` 0 nothing, 1 progress, 2 log (`level`, `module`, `message`), 3 island, 4
/// done (`mradm_engine_render_result`). Islands: 1 MP4 packager probe, 2 Apple prepare, 3 Apple
/// render, 4 FLAC, 5 Opus MKA, 6 APAC, 7 IAMF, 8 MP4 packaging, 9 metadata; `src` / `dst` the
/// files, the Apple plan's output in `dst`.
#[repr(C)]
pub struct EngineRenderEvent {
    pub kind: u32,
    pub progress: EngineProgress,
    pub level: u32,
    pub module: Text,
    pub message: Text,
    pub island: u32,
    pub src: EnginePath,
    pub dst: EnginePath,
    pub layout: Text,
    pub bitrate: u32,
    pub drc_music: u32,
    pub caf: u32,
    pub has_lufs: u32,
    pub has_peak: u32,
    pub lufs: f64,
    pub peak_dbtp: f64,
    pub layers: *const Text,
    pub layers_len: usize,
    pub has_window: u32,
    pub window_start: u64,
    pub window_count: u64,
    pub has_meter_window: u32,
    pub meter_start: u64,
    pub meter_count: u64,
    pub has_matrix: u32,
    pub matrix: DirectMatrixView,
    pub encoder: Text,
    pub date_utc: Text,
    pub renderer: Text,
}

/// An island's answer: `code` 0 for success; the Apple render's metrics; the probed packager (0
/// none, 1 mp4box, 2 ffmpeg) and ffmpeg major version (-1 unknown).
#[repr(C)]
pub struct EngineIslandReply {
    pub code: i32,
    pub message: Text,
    pub context: Text,
    pub has_lufs: u32,
    pub has_peak: u32,
    pub lufs: f64,
    pub peak_dbtp: f64,
    pub packager: u32,
    pub ffmpeg_major: i32,
}

/// `RenderResult`: `code` 0 on success; one log line per result (empty module).
#[repr(C)]
pub struct EngineRenderResult {
    pub code: i32,
    pub message: Text,
    pub context: Text,
    pub has_output: u32,
    pub output_path: Text,
    pub has_metrics: u32,
    pub has_lufs: u32,
    pub has_peak: u32,
    pub lufs: f64,
    pub peak_dbtp: f64,
    pub logs: *const EngineLog,
    pub logs_len: usize,
    pub has_report: u32,
    pub report: Text,
}

/// A render in progress, and what its last event points at.
pub struct EngineRender {
    render: Render,
    island: Option<Island>,
    islands: Option<IslandRunner>,
    /// The Apple preparation the caller cached or this render made (then `apple_made`).
    apple_prepared: RefCell<Option<Arc<Preparation>>>,
    apple_made: Cell<bool>,
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
/// a cached preparation `cached` (may be null; a Rust backend's or the Apple island's).
/// Preparation reuse does not require a preimported scene. The handle copies what it needs.
/// Returns 1 for invalid arguments, including paths not encoded as UTF-8 or containing NUL, and
/// 6 for a panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_begin(
    request: *const EngineRenderRequest,
    islands: *const PlatformIslands,
    preimported: *const SceneHandle,
    cached: *const RenderPrepared,
    output: *mut *mut EngineRender,
) -> i32 {
    let Some(out) = (unsafe { output.as_mut() }) else {
        return 1;
    };
    *out = ptr::null_mut();
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(r), Some(p)) = (unsafe { request.as_ref() }, unsafe { islands.as_ref() }) else {
            return 1;
        };
        let (Some(request), Some(platform)) = (unsafe { request_of(r) }, unsafe {
            platform(p, Facts::Render)
        }) else {
            return 1;
        };
        let cached = unsafe { cached.as_ref() };
        let apple = cached.and_then(|c| c.apple.clone());
        let reuse = Reuse {
            scene: unsafe { preimported.as_ref() }.map(|scene| scene.scene().clone()),
            prepared: cached.and_then(|c| c.prepared.clone()),
            apple_prepared: apple.is_some(),
        };
        *out = Box::into_raw(Box::new(EngineRender {
            render: Render::new(request, platform, reuse),
            island: None,
            islands: None,
            apple_prepared: RefCell::new(apple),
            apple_made: Cell::new(false),
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

/// The texts and matrix an island record borrows besides the island itself.
#[derive(Default)]
struct IslandStorage {
    texts: Vec<Text>,
    matrix: Option<Box<DirectMatrix>>,
}

/// Fills `e` with the pending island, borrowing the handle's copy of it.
fn island_record(h: &mut EngineRender, e: &mut EngineRenderEvent) {
    let island = h.island.as_ref().expect("pending island");
    let mut storage = IslandStorage::default();
    island_fields(island, &mut storage, e);
    h.texts = storage.texts;
    h.matrix = storage.matrix;
}

/// Fills `e` with `island`, borrowing it and `storage`.
fn island_fields(island: &Island, storage: &mut IslandStorage, e: &mut EngineRenderEvent) {
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
                storage.matrix = Some(matrix);
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
            storage.texts = layers.iter().map(|l| Text::new(l)).collect();
            e.layers = storage.texts.as_ptr();
            e.layers_len = storage.texts.len();
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
        let runner = h.islands.as_ref().map(|islands| Runner {
            islands,
            apple: Apple::new(islands.apple),
            prepared: &h.apple_prepared,
            made: &h.apple_made,
        });
        let islands = runner.as_ref().map(|r| r as &dyn render::Islands);
        match h.render.next_with(cancelled != 0, &FfiHost, islands) {
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
        let reply = unsafe { reply_of(r) };
        h.island = None;
        h.matrix = None;
        h.render.resume(reply);
        0
    }))
    .unwrap_or(6)
}

/// An island's reply, copying its texts.
pub(crate) unsafe fn reply_of(r: &EngineIslandReply) -> Reply {
    let error = (r.code != 0).then(|| {
        request::Error::new(
            r.code,
            unsafe { string(&r.message) }.unwrap_or_default(),
            unsafe { string(&r.context) }.unwrap_or_default(),
        )
    });
    Reply {
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
    }
}

/// The caller's side of an island run inside `mradm_engine_render_next`: `cancelled` polls its
/// stop token (possibly from another thread), `progress` receives an event mapped into the
/// render's range.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IslandContext {
    pub user: *mut c_void,
    pub cancelled: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    pub progress: Option<unsafe extern "C" fn(*mut c_void, *const EngineProgress)>,
    /// A line the island writes (level: `adm::LogLevel`).
    pub log: Option<unsafe extern "C" fn(*mut c_void, u32, Text, Text)>,
}

/// A library's islands: `run` returns 0 when it ran the island (any failure in the reply) and
/// nonzero to leave it to the caller as an island event; `apple` (may be null) runs the Apple
/// backend's islands.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IslandRunner {
    pub run: Option<
        unsafe extern "C" fn(
            *const EngineRenderEvent,
            *const IslandContext,
            *mut EngineIslandReply,
        ) -> i32,
    >,
    pub driver: IslandContext,
    pub apple: *const AppleIslands,
}

/// An island's context while it runs: the caller's context and the render's progress mapping.
struct Bridge<'a> {
    driver: IslandContext,
    map: &'a dyn Fn(&Progress) -> Progress,
}

unsafe extern "C" fn bridge_cancelled(user: *mut c_void) -> u32 {
    let b = unsafe { &*user.cast::<Bridge<'_>>() };
    b.driver
        .cancelled
        .map_or(0, |f| unsafe { f(b.driver.user) })
}

unsafe extern "C" fn bridge_progress(user: *mut c_void, event: *const EngineProgress) {
    let b = unsafe { &*user.cast::<Bridge<'_>>() };
    let (Some(f), Some(e)) = (b.driver.progress, unsafe { event.as_ref() }) else {
        return;
    };
    let mapped = mapped_progress(e, b.map);
    unsafe { f(b.driver.user, &mapped) };
}

unsafe extern "C" fn bridge_log(user: *mut c_void, level: u32, module: Text, message: Text) {
    let b = unsafe { &*user.cast::<Bridge<'_>>() };
    if let Some(f) = b.driver.log {
        unsafe { f(b.driver.user, level, module, message) };
    }
}

impl Bridge<'_> {
    fn context(&self) -> IslandContext {
        IslandContext {
            user: ptr::from_ref(self).cast_mut().cast(),
            cancelled: Some(bridge_cancelled),
            progress: Some(bridge_progress),
            log: Some(bridge_log),
        }
    }
}

/// A reply before the island writes it.
pub(crate) fn empty_reply() -> EngineIslandReply {
    EngineIslandReply {
        code: 0,
        message: Text::new(""),
        context: Text::new(""),
        has_lufs: 0,
        has_peak: 0,
        lufs: 0.,
        peak_dbtp: 0.,
        packager: 0,
        ffmpeg_major: -1,
    }
}

/// The registered islands of one step: the audio runner and the Apple table, with the render's
/// Apple preparation.
struct Runner<'a> {
    islands: &'a IslandRunner,
    apple: Option<Apple>,
    prepared: &'a RefCell<Option<Arc<Preparation>>>,
    made: &'a Cell<bool>,
}

impl Runner<'_> {
    fn apple(
        &self,
        apple: Apple,
        island: &Island,
        scene: Option<&RenderScene>,
        bridge: &Bridge<'_>,
    ) -> Reply {
        let (Island::ApplePrepare(plan) | Island::AppleRender(plan)) = island else {
            unreachable!("an Apple island");
        };
        let settings = Settings::new(
            &plan.input_path,
            &plan.output_layout,
            plan.matrix.as_ref(),
            &plan.options,
        );
        let context = bridge.context();
        let mut reply = empty_reply();
        if matches!(island, Island::ApplePrepare(_)) {
            if let Some(p) = apple.prepare(&settings, scene, &context, &mut reply) {
                *self.prepared.borrow_mut() = Some(p);
                self.made.set(true);
            }
        } else if let Some(p) = self.prepared.borrow().clone() {
            apple.render(
                &p,
                &settings,
                &plan.output_path.utf8,
                plan.window,
                &context,
                &mut reply,
            );
        } else {
            return Reply {
                error: Some(request::Error::new(
                    6,
                    "the Apple preparation is missing",
                    "",
                )),
                ..Reply::default()
            };
        }
        unsafe { reply_of(&reply) }
    }
}

impl render::Islands for Runner<'_> {
    fn run(
        &self,
        island: &Island,
        scene: Option<&RenderScene>,
        map: &dyn Fn(&Progress) -> Progress,
    ) -> Option<Reply> {
        let bridge = Bridge {
            driver: self.islands.driver,
            map,
        };
        if matches!(island, Island::ApplePrepare(_) | Island::AppleRender(_)) {
            return self
                .apple
                .map(|apple| self.apple(apple, island, scene, &bridge));
        }
        let run = self.islands.run?;
        let mut storage = IslandStorage::default();
        let mut e = empty_event();
        island_fields(island, &mut storage, &mut e);
        let context = bridge.context();
        let mut reply = empty_reply();
        if unsafe { run(&e, &context, &mut reply) } != 0 {
            return None;
        }
        Some(unsafe { reply_of(&reply) })
    }
}

/// Runs the islands `runner` accepts inside `mradm_engine_render_next` (copied); null returns
/// every island to the caller again. Returns 1 for a null handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_render_set_islands(
    handle: *mut EngineRender,
    runner: *const IslandRunner,
) -> i32 {
    let Some(h) = (unsafe { handle.as_mut() }) else {
        return 1;
    };
    h.islands = unsafe { runner.as_ref() }.copied();
    0
}

/// `e` mapped by `map`; the message borrows `e`'s unless it was empty.
fn mapped_progress(e: &EngineProgress, map: &dyn Fn(&Progress) -> Progress) -> EngineProgress {
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
    let mut out = progress_record(&map(&raw));
    if e.message.len != 0 {
        out.message = e.message;
    }
    out
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
    let prepared = h.render.take_prepared();
    let apple = h
        .apple_made
        .get()
        .then(|| h.apple_prepared.borrow().clone())
        .flatten();
    if prepared.is_none() && apple.is_none() {
        return ptr::null_mut();
    }
    Box::into_raw(Box::new(RenderPrepared {
        logs: Vec::new(),
        log_views: Vec::new(),
        error: None,
        report: None,
        prepared,
        apple,
    }))
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

    mod apple {
        use super::*;
        use crate::render::apple_island::{ApplePrepared, AppleSettings};
        use mradm_engine::render::{AppleOptions, ApplePlan};
        use mradm_render::offline::Window;
        use std::sync::Mutex;

        static CALLS: Mutex<Vec<String>> = Mutex::new(Vec::new());

        fn call(line: String) {
            CALLS.lock().unwrap().push(line);
        }

        fn text(t: &Text) -> String {
            String::from_utf8(unsafe { crate::render::bytes(t) }.unwrap().to_vec()).unwrap()
        }

        unsafe extern "C" fn prepare(
            s: *const AppleSettings,
            scene: *mut SceneHandle,
            context: *const IslandContext,
            out: *mut *mut ApplePrepared,
            reply: *mut EngineIslandReply,
        ) -> i32 {
            let (s, c) = unsafe { (&*s, &*context) };
            call(format!("prepare {}", settings_text(s)));
            if !scene.is_null() {
                drop(unsafe { Box::from_raw(scene) });
            }
            unsafe { (c.log.unwrap())(c.user, 2, Text::new("apple"), Text::new("prepared")) };
            let raw = progress_record(&Progress {
                stage: 2,
                operation: 0,
                fraction: 0.,
                stage_fraction: 0.5,
                current: 1,
                total: 2,
                message: "",
            });
            unsafe { (c.progress.unwrap())(c.user, &raw) };
            if crate::render::apple_island::settings_fields(s).layout == "bad" {
                let r = unsafe { &mut *reply };
                r.code = 4;
                r.message = Text::new("unsupported layout");
                return 0;
            }
            unsafe { *out = Box::into_raw(Box::new(7u32)).cast() };
            0
        }

        unsafe extern "C" fn render(
            p: *const ApplePrepared,
            s: *const AppleSettings,
            output: Text,
            has_window: u32,
            start: u64,
            count: u64,
            _: *const IslandContext,
            reply: *mut EngineIslandReply,
        ) -> i32 {
            let prepared = unsafe { *p.cast::<u32>() };
            call(format!(
                "render {prepared} {} {} {has_window} {start} {count}",
                text(&output),
                settings_text(unsafe { &*s })
            ));
            let r = unsafe { &mut *reply };
            r.has_lufs = 1;
            r.lufs = -20.;
            0
        }

        unsafe extern "C" fn destroy(p: *mut ApplePrepared) {
            call(format!("destroy {}", unsafe { *p.cast::<u32>() }));
            drop(unsafe { Box::from_raw(p.cast::<u32>()) });
        }

        fn settings_text(s: &AppleSettings) -> String {
            let s = crate::render::apple_island::settings_fields(s);
            format!("{s:?}")
        }

        static TABLE: AppleIslands = crate::render::apple_island::test_table(
            Some(prepare),
            Some(render),
            None,
            Some(destroy),
        );

        #[derive(Default)]
        struct Driver {
            logs: Vec<String>,
            fractions: Vec<f64>,
        }

        unsafe extern "C" fn driver_progress(user: *mut c_void, e: *const EngineProgress) {
            let d = unsafe { &mut *user.cast::<Driver>() };
            d.fractions.push(unsafe { (*e).fraction });
        }

        unsafe extern "C" fn driver_log(
            user: *mut c_void,
            level: u32,
            module: Text,
            message: Text,
        ) {
            let d = unsafe { &mut *user.cast::<Driver>() };
            d.logs
                .push(format!("{level} {} {}", text(&module), text(&message)));
        }

        fn plan(layout: &str) -> ApplePlan {
            let path = |p: &str| PathText {
                utf8: p.into(),
                display: p.into(),
            };
            ApplePlan {
                input_path: path("in.wav"),
                output_path: path("out.wav"),
                output_layout: layout.into(),
                window: Some(Window {
                    start: 10,
                    count: 20,
                }),
                meter_window: None,
                matrix: None,
                options: AppleOptions {
                    routing: 2,
                    split_power: true,
                    speaker_spread: 1,
                    binaural_spread: 3,
                    spatial_preset: 4,
                    speaker_rendering_flags: true,
                    orientation: [10., 5., -3.],
                },
            }
        }

        #[test]
        fn the_apple_runner_prepares_renders_and_keeps_the_preparation() {
            let mut driver = Driver::default();
            let runner = IslandRunner {
                run: None,
                driver: IslandContext {
                    user: ptr::from_mut(&mut driver).cast(),
                    cancelled: None,
                    progress: Some(driver_progress),
                    log: Some(driver_log),
                },
                apple: &TABLE,
            };
            let prepared = RefCell::new(None);
            let made = Cell::new(false);
            let r = Runner {
                islands: &runner,
                apple: Apple::new(runner.apple),
                prepared: &prepared,
                made: &made,
            };
            let map = |p: &Progress| Progress {
                fraction: 0.25 + p.stage_fraction,
                ..*p
            };
            let islands: &dyn render::Islands = &r;
            let reply = islands.run(
                &Island::ApplePrepare(Box::new(plan("binaural"))),
                None,
                &map,
            );
            assert!(reply.unwrap().error.is_none());
            assert!(made.get() && prepared.borrow().is_some());
            let reply = islands
                .run(&Island::AppleRender(Box::new(plan("binaural"))), None, &map)
                .unwrap();
            assert_eq!((reply.error, reply.metrics.lufs), (None, Some(-20.)));
            // Other islands without an audio runner are returned.
            assert!(islands.run(&Island::ProbeMp4Packager, None, &map).is_none());
            let failed = islands
                .run(&Island::ApplePrepare(Box::new(plan("bad"))), None, &map)
                .unwrap();
            assert_eq!(
                failed.error.map(|e| (e.code, e.message)),
                Some((4, "unsupported layout".into()))
            );
            prepared.borrow_mut().take();
            let fields = "AppleFields { input: \"in.wav\", layout: \"binaural\", routing: 2, \
                          has_matrix: 0, split_power: 1, speaker_spread: 1, binaural_spread: 3, \
                          spatial_preset: 4, speaker_rendering_flags: 1, orientation: [10.0, 5.0, -3.0] }";
            let bad = fields.replace("\"binaural\"", "\"bad\"");
            assert_eq!(
                *CALLS.lock().unwrap(),
                [
                    format!("prepare {fields}"),
                    format!("render 7 out.wav {fields} 1 10 20"),
                    format!("prepare {bad}"),
                    "destroy 7".to_owned(),
                ]
            );
            assert_eq!(driver.logs, ["2 apple prepared", "2 apple prepared"]);
            assert_eq!(driver.fractions, [0.75, 0.75]);

            let empty = RefCell::new(None);
            let missing = Runner {
                prepared: &empty,
                ..r
            };
            let reply = (&missing as &dyn render::Islands)
                .run(&Island::AppleRender(Box::new(plan("binaural"))), None, &map)
                .unwrap();
            assert_eq!(
                reply.error.unwrap().message,
                "the Apple preparation is missing"
            );
            let none = Runner {
                apple: None,
                ..missing
            };
            assert!(
                (&none as &dyn render::Islands)
                    .run(
                        &Island::ApplePrepare(Box::new(plan("binaural"))),
                        None,
                        &map
                    )
                    .is_none()
            );
        }
    }
}
