//! Private boundary of the backend preparation and the offline render step
//! (`mradm-engine` `backend` and `offline`). This layer is the engine's [`Host`]: the room
//! floating-point scope, the room resources with their CUDA selection and the room switch.
use crate::adm::Text;
use crate::engine::{EngineLog, EngineStatus};
use crate::render::{RenderMatrixRoute, RenderPrepared, bytes, matrix_routes};
use crate::room::RoomProfileHandle;
use crate::scene::SceneHandle;
use mradm_dsp::room::loading::Package;
use mradm_dsp::room::screen::{Mapping, RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{
    self, BinauralSpreadMode, Host, Kind, RoomRequest, Routing, Screens, Sofa, SpeakerSpread,
};
use mradm_engine::offline::{Event, Run, Settings};
use mradm_engine::request;
use mradm_render::offline::Window;
use mradm_render::room;
use mradm_render::room_runtime::{Prepare, Resources, Scope};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr,
    sync::Arc,
};

/// The platform services of the engine (formerly the C++ `RoomRenderer::prepare` calls).
pub struct FfiHost;

impl Host for FfiHost {
    fn validate_screens(&self, screens: &RoomScreens) -> Result<(), (i32, String)> {
        crate::room::scope(&mut || Mapping::new(*screens).map(drop))
            .map_err(|(code, message)| (code, message.to_owned()))
    }

    fn prepare_room(
        &self,
        package: &Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        metres: f64,
    ) -> Result<Resources, (i32, String)> {
        let mut out = None;
        crate::room::scope(&mut || {
            let (set, cuda, metadata) =
                crate::room::prepare_set(package, hrir, sofa, None, quality, metres)?;
            // `mradm_room_prepare_quality` also described the set; keep its failure.
            crate::room::description(&set, metres, metadata)?;
            out = Some(Resources { set, cuda });
            Ok(())
        })
        .map_err(|(code, message)| (code, message.to_owned()))?;
        Ok(out.expect("prepared in scope"))
    }

    fn room_polar(&self) -> room::PolarConversion {
        crate::render::room_polar
    }

    fn room_scope(&self) -> Scope {
        crate::room::scope
    }

    fn room_switch(&self) -> Prepare {
        crate::render::room_ffi::switch()
    }
    #[cfg(windows)]
    fn room_worker(&self, active: bool) {
        crate::scene_worker::set_active(active);
    }
    #[cfg(windows)]
    fn room_wait(&self, duration: std::time::Duration) {
        crate::scene_worker::wait(duration);
    }
}

/// `RenderPlan` as `mradm_engine::backend::Request`. `kind`: `render_common::EngineBackend`;
/// `routing`, `speaker_spread`, `binaural_spread`: the C++ enum values; `room_profile` may be null.
#[repr(C)]
pub struct EnginePrepareRequest {
    kind: u32,
    layout_id: Text,
    apple_geometry: u32,
    routing: u32,
    has_matrix: u32,
    matrix: *const RenderMatrixRoute,
    matrix_len: usize,
    speaker_spread: u32,
    binaural_spread: u32,
    triple_balance_d: u32,
    split_power: u32,
    smoothing_frames: u32,
    default_interp_ms: u32,
    input_path: Text,
    has_sofa: u32,
    sofa_path: Text,
    sofa_file_name: Text,
    has_reference: u32,
    reference: RoomScreenGeometry,
    has_reproduction: u32,
    reproduction: RoomScreenGeometry,
    room_profile: *const RoomProfileHandle,
    room_hrir: Text,
    room_metres: f64,
    room_quality: u32,
}

/// The status and log lines of a preparation, the Triple Balance report (`has_report`), and the
/// prepared plan's track, input and output counts and sample rate. Valid while the handle lives.
#[repr(C)]
pub struct EnginePreparedView {
    status: EngineStatus,
    has_report: u32,
    report: Text,
    tracks: usize,
    inputs: usize,
    outputs: usize,
    sample_rate: u32,
}

pub(crate) fn log_views(logs: &[request::Log]) -> Vec<EngineLog> {
    logs.iter()
        .map(|l| EngineLog {
            level: l.level as u32,
            module: Text::new(l.module),
            message: Text::new(&l.message),
        })
        .collect()
}

pub(crate) fn status_of(error: Option<&request::Error>, logs: &[EngineLog]) -> EngineStatus {
    let (code, message, context) = error.map_or((0, Text::new(""), Text::new("")), |e| {
        (e.code, Text::new(&e.message), Text::new(&e.context))
    });
    EngineStatus {
        code,
        message,
        context,
        logs: logs.as_ptr(),
        logs_len: logs.len(),
    }
}

/// Prepares a backend on the policy-applied scene. Unless the arguments are invalid (status 1,
/// `*output` null) `*output` and `*view` are set, also for a rejected preparation; the log lines
/// precede the error. Destroy with `mradm_render_prepared_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_prepare(
    scene: *const SceneHandle,
    request: *const EnginePrepareRequest,
    output: *mut *mut RenderPrepared,
    view: *mut EnginePreparedView,
) -> i32 {
    let (Some(out), Some(view)) = (unsafe { output.as_mut() }, unsafe { view.as_mut() }) else {
        return 1;
    };
    *out = ptr::null_mut();
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(scene), Some(r)) = (unsafe { scene.as_ref() }, unsafe { request.as_ref() })
        else {
            return 1;
        };
        let parsed = (|| {
            let matrix = unsafe { matrix_routes(r.matrix, r.matrix_len)? };
            let text = |t: &Text| unsafe { bytes(t) };
            Ok::<_, mradm_render::routing::Error>((
                matrix,
                text(&r.layout_id)?,
                text(&r.input_path)?,
                text(&r.sofa_path)?,
                text(&r.sofa_file_name)?,
                text(&r.room_hrir)?,
            ))
        })();
        let Ok((matrix, layout_id, input_path, sofa_path, sofa_file_name, room_hrir)) = parsed
        else {
            return 1;
        };
        let (Some(kind), Some(routing), Some(speaker_spread), Some(binaural_spread)) = (
            Kind::from_u32(r.kind),
            routing_of(r.routing),
            speaker_spread_of(r.speaker_spread),
            binaural_spread_of(r.binaural_spread),
        ) else {
            return 1;
        };
        let request = backend::Request {
            kind,
            layout_id,
            apple_geometry: r.apple_geometry != 0,
            routing,
            matrix: (r.has_matrix != 0).then_some(&matrix[..]),
            speaker_spread,
            binaural_spread,
            triple_balance_d: r.triple_balance_d != 0,
            split_power: r.split_power != 0,
            smoothing_frames: r.smoothing_frames,
            default_interp_ms: r.default_interp_ms,
            input_path,
            sofa: (r.has_sofa != 0).then_some(Sofa {
                path: sofa_path,
                file_name: sofa_file_name,
            }),
            screens: Screens {
                has_reference: r.has_reference != 0,
                reference: r.reference,
                has_reproduction: r.has_reproduction != 0,
                reproduction: r.reproduction,
            },
            room: RoomRequest {
                package: unsafe { crate::room::package(r.room_profile) },
                hrir: room_hrir,
                metres: r.room_metres,
                quality: r.room_quality,
            },
        };
        let preparation = backend::prepare(scene.scene(), &request, &FfiHost);
        let (prepared, error) = match preparation.outcome.result {
            Ok(p) => (Some(p), None),
            Err(e) => (None, Some(e)),
        };
        let mut handle = Box::new(RenderPrepared {
            logs: preparation.outcome.logs,
            log_views: Vec::new(),
            error,
            report: preparation.report,
            prepared,
        });
        handle.log_views = log_views(&handle.logs);
        let plan = handle.plan();
        *view = EnginePreparedView {
            status: status_of(handle.error.as_ref(), &handle.log_views),
            has_report: u32::from(handle.report.is_some()),
            report: Text::new(handle.report.as_deref().unwrap_or("")),
            tracks: plan.map_or(0, |p| p.bindings.len()),
            inputs: plan.map_or(0, |p| p.inputs),
            outputs: plan.map_or(0, |p| p.outputs),
            sample_rate: plan.map_or(0, |p| {
                p.room.as_ref().map_or(p.sample_rate, |r| r.sample_rate)
            }),
        };
        let code = view.status.code;
        *out = Box::into_raw(handle);
        code
    }))
    .unwrap_or(6)
}

fn routing_of(value: u32) -> Option<Routing> {
    Some(match value {
        0 => Routing::Automatic,
        1 => Routing::Label,
        2 => Routing::Position,
        3 => Routing::Matrix,
        _ => return None,
    })
}

pub(crate) fn speaker_spread_of(value: u32) -> Option<SpeakerSpread> {
    Some(match value {
        0 => SpeakerSpread::Automatic,
        1 => SpeakerSpread::None,
        2 => SpeakerSpread::Mdap,
        _ => return None,
    })
}

pub(crate) fn binaural_spread_of(value: u32) -> Option<BinauralSpreadMode> {
    Some(match value {
        0 => BinauralSpreadMode::Automatic,
        1 => BinauralSpreadMode::None,
        2 => BinauralSpreadMode::Cloud,
        3 => BinauralSpreadMode::SafSpreader,
        _ => return None,
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_prepared_destroy(handle: *mut RenderPrepared) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The `RenderPlan` fields of one offline render (`mradm_engine::offline::Settings`).
#[repr(C)]
pub struct EngineOfflineSettings {
    input_path: Text,
    output_path: Text,
    input_display: Text,
    output_display: Text,
    output_layout: Text,
    inputs: usize,
    frames: u64,
    sample_rate: u32,
    default_interp_ms: u32,
    smoothing_frames: u32,
    has_window: u32,
    window_start: u64,
    window_count: u64,
    has_meter_window: u32,
    meter_start: u64,
    meter_count: u64,
    pose: [f32; 3],
}

/// One event: `kind` 0 nothing, 1 progress (`stage` … `message`), 2 log (`level`, `module`,
/// `message`), 3 done (the metrics). The texts live until the next call.
#[repr(C)]
pub struct EngineOfflineEvent {
    kind: u32,
    stage: u32,
    operation: u32,
    fraction: f64,
    stage_fraction: f64,
    current: u64,
    total: u64,
    level: u32,
    module: Text,
    message: Text,
    has_lufs: u32,
    has_peak: u32,
    lufs: f64,
    peak_dbtp: f64,
}

/// An offline render in progress; it keeps its preparation alive.
pub struct EngineOffline {
    run: Run,
    message: String,
    error: Option<request::Error>,
}

/// Starts the offline render of a successful preparation; nothing is opened until the first
/// `mradm_engine_offline_next`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_offline_open(
    prepared: *const RenderPrepared,
    settings: *const EngineOfflineSettings,
    output: *mut *mut EngineOffline,
) -> i32 {
    let Some(out) = (unsafe { output.as_mut() }) else {
        return 1;
    };
    *out = ptr::null_mut();
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(prepared), Some(s)) = (
            unsafe { prepared.as_ref() }.and_then(|p| p.prepared.clone()),
            unsafe { settings.as_ref() },
        ) else {
            return 1;
        };
        let texts = unsafe {
            (
                bytes(&s.input_path),
                bytes(&s.output_path),
                bytes(&s.input_display),
                bytes(&s.output_display),
                bytes(&s.output_layout),
            )
        };
        let (
            Ok(input_path),
            Ok(output_path),
            Ok(input_display),
            Ok(output_display),
            Ok(output_layout),
        ) = texts
        else {
            return 1;
        };
        let window = |has: u32, start, count| (has != 0).then_some(Window { start, count });
        let run = Run::new(
            Arc::clone(&prepared),
            &Settings {
                input_path,
                output_path,
                input_display,
                output_display,
                output_layout,
                inputs: s.inputs,
                frames: s.frames,
                sample_rate: s.sample_rate,
                default_interp_ms: s.default_interp_ms,
                smoothing_frames: s.smoothing_frames,
                window: window(s.has_window, s.window_start, s.window_count),
                meter_window: window(s.has_meter_window, s.meter_start, s.meter_count),
                pose: s.pose,
            },
        );
        *out = Box::into_raw(Box::new(EngineOffline {
            run,
            message: String::new(),
            error: None,
        }));
        0
    }))
    .unwrap_or(6)
}

/// The next event. On an error (the status code) `*status` holds its message and context until
/// the next call; the run is then spent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_offline_next(
    handle: *mut EngineOffline,
    cancelled: u32,
    event: *mut EngineOfflineEvent,
    status: *mut EngineStatus,
) -> i32 {
    let (Some(h), Some(event), Some(status)) = (
        unsafe { handle.as_mut() },
        unsafe { event.as_mut() },
        unsafe { status.as_mut() },
    ) else {
        return 1;
    };
    *event = EngineOfflineEvent {
        kind: 0,
        stage: 0,
        operation: 0,
        fraction: 0.,
        stage_fraction: 0.,
        current: 0,
        total: 0,
        level: 0,
        module: Text::new(""),
        message: Text::new(""),
        has_lufs: 0,
        has_peak: 0,
        lufs: 0.,
        peak_dbtp: 0.,
    };
    let result = catch_unwind(AssertUnwindSafe(|| h.run.next(cancelled != 0, &FfiHost)))
        .unwrap_or_else(|_| Err(request::Error::new(6, "Rust render panic", "")));
    h.error = None;
    match result {
        Ok(Event::Continue) => {}
        Ok(Event::Progress(p)) => {
            event.kind = 1;
            event.stage = p.stage;
            event.operation = p.operation;
            event.fraction = p.fraction;
            event.stage_fraction = p.stage_fraction;
            event.current = p.current;
            event.total = p.total;
            event.message = Text::new(p.message);
        }
        Ok(Event::Log(l)) => {
            h.message = l.message;
            event.kind = 2;
            event.level = l.level as u32;
            event.module = Text::new(l.module);
            event.message = Text::new(&h.message);
        }
        Ok(Event::Done(m)) => {
            event.kind = 3;
            event.has_lufs = u32::from(m.lufs.is_some());
            event.has_peak = u32::from(m.peak_dbtp.is_some());
            event.lufs = m.lufs.unwrap_or(0.);
            event.peak_dbtp = m.peak_dbtp.unwrap_or(0.);
        }
        Err(e) => h.error = Some(e),
    }
    *status = status_of(h.error.as_ref(), &[]);
    status.code
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_offline_destroy(handle: *mut EngineOffline) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}
