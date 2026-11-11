//! Private file-stream boundary: one handle for every Rust backend's
//! stream (`mradm-engine::stream`). Validation may overlap the worker and borrows only the
//! immutable validator; serialize every other call on the worker and destroy after all return.
//! Errors carry the stream locator (`MradmRenderLocation`) like the offline driver's.
use super::room_ffi::{RenderRoomControl, control_handle};
use super::*;
use crate::engine::{EngineLog, EnginePlatform, EngineStatus, platform};
use crate::engine_render::{FfiHost, log_views, status_of};
use crate::engine_service::{EngineRenderRequest, request_of};
use crate::scene::{DirectMatrix, DirectMatrixView, SceneHandle};
use mradm_engine::stream::{self as engine_stream, Body, Monitor, Settings, Validator};
use mradm_render::binaural::Spread;

/// `RenderPlan`'s input format and stream controls; `spread` is the binaural spread (0 cloud,
/// 1 none, 2 SAF spreader).
#[repr(C)]
pub struct EngineStreamSettings {
    channels: usize,
    frames: u64,
    sample_rate: u32,
    default_interp_ms: u32,
    smoothing_frames: u32,
    spread: u32,
    pose: [f32; 3],
}

/// `IRenderStream`'s format queries; `binaural` is 1 when the output layout is "binaural".
#[repr(C)]
pub struct EngineStreamInfo {
    pub(super) outputs: usize,
    pub(super) sample_rate: u32,
    pub(super) binaural: u32,
    pub(super) requires_preroll: u32,
}

pub struct EngineStream {
    validator: Validator,
    body: Body,
}

impl EngineStream {
    #[cfg(test)]
    pub(super) fn new(validator: Validator, body: Body) -> Self {
        Self { validator, body }
    }

    /// The validator and body a monitor takes over (`release_engine_stream`).
    pub(super) fn into_parts(self) -> (Validator, Body) {
        (self.validator, self.body)
    }
}

pub(super) unsafe fn body<'a>(handle: *mut EngineStream) -> offline::Result<&'a mut Body> {
    if handle.is_null() {
        return Err(driver_error("无效流式渲染句柄"));
    }
    // SAFETY: Worker calls are serialized. Borrow only the body: an enclosing &mut handle would
    // alias the control thread's immutable validator.
    Ok(unsafe { &mut (*handle).body })
}

/// A worker-side query (the C++ stream's const members).
unsafe fn shared_body<'a>(handle: *const EngineStream) -> Option<&'a Body> {
    (!handle.is_null()).then(|| unsafe { &(*handle).body })
}

unsafe fn validator<'a>(handle: *const EngineStream) -> Option<&'a Validator> {
    // SAFETY: The validator is immutable for the handle's lifetime; do not borrow the handle,
    // which also holds the body the worker may be mutating.
    (!handle.is_null()).then(|| unsafe { &(*handle).validator })
}

fn spread_of(value: u32) -> Spread {
    match value {
        1 => Spread::None,
        2 => Spread::SafSpreader,
        _ => Spread::Cloud,
    }
}

/// Opens the file stream of a preparation (`mradm_engine_prepare`) on the input (UTF-8 path).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_open(
    prepared: *const RenderPrepared,
    path: *const u8,
    path_len: usize,
    settings: *const EngineStreamSettings,
    output: *mut *mut EngineStream,
    info: *mut EngineStreamInfo,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(s), Some(p), Some(info)) = (
            unsafe { output.as_mut() },
            unsafe { settings.as_ref() },
            unsafe { prepared.as_ref() }.and_then(RenderPrepared::plan),
            unsafe { info.as_mut() },
        ) else {
            return Err(driver_error("无效流式渲染参数"));
        };
        *out = ptr::null_mut();
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let (validator, body, i) = engine_stream::open(
            p,
            &Settings {
                path,
                channels: s.channels,
                frames: s.frames,
                sample_rate: s.sample_rate,
                default_interp_ms: s.default_interp_ms,
                smoothing_frames: s.smoothing_frames,
                spread: spread_of(s.spread),
                pose: s.pose,
            },
            &FfiHost,
        )?;
        *info = EngineStreamInfo {
            outputs: i.outputs,
            sample_rate: i.sample_rate,
            binaural: u32::from(i.binaural),
            requires_preroll: u32::from(i.requires_preroll),
        };
        *out = Box::into_raw(Box::new(EngineStream { validator, body }));
        Ok(())
    })
}

/// Fills up to `frames` interleaved frames of `output`; `*produced` is less only at the end.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_process(
    handle: *mut EngineStream,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    produced: *mut usize,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { produced.as_mut() }.ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = 0;
        let s = unsafe { body(handle)? };
        let buffer = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = s.process(buffer, frames)?;
        Ok(())
    })
}

/// Starts a seek; `cancelled` is the stop token sampled before the call. `*pending` (set unless
/// the call fails) asks for another `mradm_engine_stream_seek_step`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_seek_begin(
    handle: *mut EngineStream,
    frame: u64,
    cancelled: u32,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { body(handle)? }.seek_begin(frame, cancelled != 0)?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_seek_step(
    handle: *mut EngineStream,
    cancelled: u32,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { body(handle)? }.seek_step(cancelled != 0)?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

/// A rejection fills `message` and `context` (the object id) like the other boundaries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_validate(
    handle: *const EngineStream,
    values: *const RenderLiveOverride,
    values_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let v = unsafe { validator(handle) }.ok_or_else(|| bad("无效流式渲染句柄"))?;
        let o = unsafe { live_overrides(values, values_len) }.map_err(|e| bad(&e.message))?;
        v.validate(&o).map_err(|(code, message, context)| Error {
            code,
            message: message.into_bytes(),
            context,
        })
    })
}

/// Replaces the live edits; nonzero for invalid input or a rejected edit (the room latches its
/// rejection for the next `process` instead).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_overrides(
    handle: *mut EngineStream,
    values: *const RenderLiveOverride,
    values_len: usize,
) -> i32 {
    driver_boundary(ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), || {
        let o = unsafe { live_overrides(values, values_len)? };
        unsafe { body(handle)? }.set_overrides(&o)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_pose(
    handle: *mut EngineStream,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Ok(s) = unsafe { body(handle) } {
        s.set_pose([yaw, pitch, roll]);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_tail(handle: *const EngineStream) -> u64 {
    unsafe { shared_body(handle) }.map_or(0, Body::tail_frames)
}

/// A new room control handle; null for another backend or a null stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_control(
    handle: *const EngineStream,
) -> *mut RenderRoomControl {
    unsafe { shared_body(handle) }
        .and_then(Body::control)
        .map_or(ptr::null_mut(), control_handle)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_stream_destroy(handle: *mut EngineStream) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

// ── The monitor's stream opening ────────────────────────────────────────────────────────────

/// What `mradm_engine_monitor_open` opened: kind 0 a rejection (status), 1 a stream (info and the
/// plan's layout; take it with `mradm_engine_monitor_take_stream`), 2 the Apple backend over
/// `layout` and the matrix.
/// The texts live until the handle is destroyed.
#[repr(C)]
pub struct EngineMonitorView {
    status: EngineStatus,
    kind: u32,
    info: EngineStreamInfo,
    layout: Text,
    has_matrix: u32,
    matrix: DirectMatrixView,
}

pub struct EngineMonitor {
    logs: Vec<mradm_engine::request::Log>,
    log_views: Vec<EngineLog>,
    error: Option<mradm_engine::request::Error>,
    stream: Option<Box<EngineStream>>,
    layout: String,
    matrix: Option<Box<DirectMatrix>>,
}

/// Opens the monitor's stream for `request` over the session's policy-applied `scene` (may be
/// null, which the opening rejects in its order). Returns 1 for invalid arguments (including
/// paths not encoded as UTF-8 or containing NUL), 6 for a panic, and otherwise 0 with `*output`
/// and `*view` set; the view's status carries the rejection and the log lines either way.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_open(
    request: *const EngineRenderRequest,
    platform_record: *const EnginePlatform,
    scene: *const SceneHandle,
    system_spatial: u32,
    output: *mut *mut EngineMonitor,
    view: *mut EngineMonitorView,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(out), Some(view), Some(r), Some(p)) = (
            unsafe { output.as_mut() },
            unsafe { view.as_mut() },
            unsafe { request.as_ref() },
            unsafe { platform_record.as_ref() },
        ) else {
            return 1;
        };
        *out = ptr::null_mut();
        let (Some(request), Some(platform)) = (unsafe { request_of(r) }, unsafe { platform(p) })
        else {
            return 1;
        };
        let scene = unsafe { scene.as_ref() }.map(SceneHandle::scene);
        let opened = mradm_engine::stream::open_monitor(
            &request,
            system_spatial != 0,
            scene,
            platform.apple.is_some(),
            &FfiHost,
        );
        let mut handle = Box::new(EngineMonitor {
            logs: opened.logs,
            log_views: Vec::new(),
            error: None,
            stream: None,
            layout: String::new(),
            matrix: None,
        });
        let mut kind = 0;
        let mut info = EngineStreamInfo {
            outputs: 0,
            sample_rate: 0,
            binaural: 0,
            requires_preroll: 0,
        };
        match opened.result {
            Err(e) => handle.error = Some(e),
            Ok(Monitor::Stream {
                validator,
                body,
                info: i,
                layout,
            }) => {
                kind = 1;
                handle.layout = layout;
                info = EngineStreamInfo {
                    outputs: i.outputs,
                    sample_rate: i.sample_rate,
                    binaural: u32::from(i.binaural),
                    requires_preroll: u32::from(i.requires_preroll),
                };
                handle.stream = Some(Box::new(EngineStream { validator, body }));
            }
            Ok(Monitor::Apple { layout, matrix }) => {
                kind = 2;
                handle.layout = layout;
                handle.matrix = matrix.map(DirectMatrix::new);
            }
        }
        handle.log_views = log_views(&handle.logs);
        *view = EngineMonitorView {
            status: status_of(handle.error.as_ref(), &handle.log_views),
            kind,
            info,
            layout: Text::new(&handle.layout),
            has_matrix: u32::from(handle.matrix.is_some()),
            matrix: handle
                .matrix
                .as_ref()
                .map_or_else(DirectMatrix::empty_view, |m| m.view()),
        };
        *out = Box::into_raw(handle);
        0
    }))
    .unwrap_or(6)
}

/// The opened stream (null unless the view's kind was 1 and it was not taken yet); destroy it
/// with `mradm_engine_stream_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_take_stream(
    handle: *mut EngineMonitor,
) -> *mut EngineStream {
    unsafe { handle.as_mut() }
        .and_then(|h| h.stream.take())
        .map_or(ptr::null_mut(), Box::into_raw)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_destroy(handle: *mut EngineMonitor) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}
