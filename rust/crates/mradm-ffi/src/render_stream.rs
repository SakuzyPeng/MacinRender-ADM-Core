//! Private file-stream boundary: one handle for every Rust backend's
//! stream (`mradm-engine::stream`). Validation may overlap the worker and borrows only the
//! immutable validator; serialize every other call on the worker and destroy after all return.
//! Errors carry the stream locator (`MradmRenderLocation`) like the offline driver's.
use super::room_ffi::{RenderRoomControl, control_handle};
use super::*;
use crate::engine_render::FfiHost;
use mradm_engine::stream::{self as engine_stream, Body, Settings, Validator};
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
