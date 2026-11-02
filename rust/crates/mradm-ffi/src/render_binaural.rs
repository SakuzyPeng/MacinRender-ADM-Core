//! The binaural HRTF state (PCM consistency 4.7 A): loading, preparation and the Scene stream
//! cache run in Rust `mradm-render::binaural_hrtf`; the C++ renderers borrow its view.
use super::*;
use mradm_dsp::hrtf_filters::Filters;
use mradm_render::binaural::Spread;
use mradm_render::binaural_hrtf::{self, Sofa, State};
use mradm_render::binaural_render as binaural;
use mradm_render::binaural_stream::{FileStream, Settings as StreamSettings};
use std::path::Path;
use std::sync::Arc;

/// A prepared HRTF state.
pub struct BinauralHrtfHandle {
    state: Arc<State>,
}

#[repr(C)]
pub struct RenderBinauralHrtfView {
    name: Text,
    sample_rate: u32,
    num_dirs: usize,
    hrir_len: usize,
    fft_size: usize,
    overlap: usize,
    /// An owned filter-bank handle for `mradm_dsp_hrtf_*` (destroy with
    /// `mradm_dsp_hrtf_filters_destroy`).
    filters: *mut Arc<Filters>,
    hrirs: *const f32,
    hrirs_len: usize,
    directions: *const f32,
    directions_len: usize,
}

#[cfg(unix)]
fn native(bytes: &[u8]) -> Option<&Path> {
    use std::os::unix::ffi::OsStrExt;
    Some(Path::new(std::ffi::OsStr::from_bytes(bytes)))
}
#[cfg(not(unix))]
fn native(bytes: &[u8]) -> Option<&Path> {
    std::str::from_utf8(bytes).ok().map(Path::new)
}

unsafe fn prepare(
    sofa: u32,
    path: Text,
    display: Text,
    file_name: Text,
    output: *mut *mut BinauralHrtfHandle,
    view: *mut RenderBinauralHrtfView,
    make: impl FnOnce(Option<&Sofa>) -> Result<Arc<State>, Error>,
) -> Result<(), Error> {
    let (Some(out), Some(view)) = (unsafe { output.as_mut() }, unsafe { view.as_mut() }) else {
        return Err(bad("无效 HRTF 参数"));
    };
    *out = ptr::null_mut();
    let state = if sofa != 0 {
        let (path, display, file_name) =
            unsafe { (bytes(&path)?, bytes(&display)?, bytes(&file_name)?) };
        let path = native(path).ok_or_else(|| Error {
            code: 3,
            message: b"SOFA file could not be opened".to_vec(),
            context: [b"path=".as_slice(), display].concat(),
        })?;
        make(Some(&Sofa {
            path,
            display,
            file_name,
        }))?
    } else {
        make(None)?
    };
    *view = RenderBinauralHrtfView {
        name: Text::new(&state.name),
        sample_rate: state.sample_rate,
        num_dirs: state.num_dirs,
        hrir_len: state.hrir_len,
        fft_size: state.fft_size,
        overlap: state.overlap,
        filters: Box::into_raw(Box::new(Arc::clone(&state.filters))),
        hrirs: state.hrirs.as_ptr(),
        hrirs_len: state.hrirs.len(),
        directions: state.directions.as_ptr(),
        directions_len: state.directions.len(),
    };
    *out = Box::into_raw(Box::new(BinauralHrtfHandle { state }));
    Ok(())
}

/// The file renderer's HRTF state outside a render (the engine prepares its own): the built-in set
/// (`sofa` 0) or a SOFA file at `input_rate`, prepared for `block`-frame OLA blocks. The view lives as long as the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_hrtf_prepare(
    sofa: u32,
    path: Text,
    display: Text,
    file_name: Text,
    input_rate: u32,
    block: u64,
    output: *mut *mut BinauralHrtfHandle,
    view: *mut RenderBinauralHrtfView,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || unsafe {
        prepare(sofa, path, display, file_name, output, view, |sofa| {
            let dataset = binaural_hrtf::load(sofa, input_rate)?;
            binaural_hrtf::build(dataset, block, false).map(Arc::new)
        })
    })
}

/// The Scene stream renderer's cached HRTF state at `sample_rate` (without impulses).
/// The Scene stream renderer's cached HRTF state at `sample_rate` (the built-in set for `sofa` 0).
/// The view lives as long as the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_hrtf_live(
    sofa: u32,
    path: Text,
    display: Text,
    file_name: Text,
    sample_rate: u32,
    output: *mut *mut BinauralHrtfHandle,
    view: *mut RenderBinauralHrtfView,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || unsafe {
        prepare(sofa, path, display, file_name, output, view, |sofa| {
            binaural_hrtf::prepare_live(sofa, sample_rate)
        })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_hrtf_destroy(handle: *mut BinauralHrtfHandle) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

// ── File streaming over an engine preparation (5.2 C1) ─────────────────────────

fn offline_error(message: &str) -> offline::Error {
    offline::Error {
        code: 1,
        message: message.to_owned(),
        location: offline::Location::None,
    }
}

fn spread_of(value: u32) -> Spread {
    match value {
        1 => Spread::None,
        2 => Spread::SafSpreader,
        _ => Spread::Cloud,
    }
}

unsafe fn binaural_of<'a>(prepared: *const RenderPrepared) -> Option<&'a Arc<binaural::Prepared>> {
    unsafe { prepared.as_ref() }
        .and_then(RenderPrepared::plan)
        .and_then(|p| p.binaural.as_ref())
}

// ── File streaming ────────────────────────────────────────────────────────────

#[repr(C)]
pub struct RenderBinauralStreamSettings {
    inputs: usize,
    frames: u64,
    sample_rate: u32,
    smoothing_frames: u32,
    /// The plan's spread mode, as in `RenderBinauralRequest`.
    spread: u32,
}

/// A binaural file stream; every call but `destroy` runs on the stream's worker.
pub struct RenderBinauralStream(FileStream);

unsafe fn binaural_stream<'a>(
    handle: *mut RenderBinauralStream,
) -> offline::Result<&'a mut FileStream> {
    unsafe { handle.as_mut() }
        .map(|h| &mut h.0)
        .ok_or_else(|| offline_error("无效双耳流句柄"))
}

/// Opens the input (UTF-8 path) for streaming a binaural preparation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_open(
    prepared: *const RenderPrepared,
    path: *const u8,
    path_len: usize,
    settings: *const RenderBinauralStreamSettings,
    output: *mut *mut RenderBinauralStream,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(s), Some(plan)) = (
            unsafe { output.as_mut() },
            unsafe { settings.as_ref() },
            unsafe { binaural_of(prepared) },
        ) else {
            return Err(offline_error("无效流式渲染参数"));
        };
        *out = ptr::null_mut();
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let stream = FileStream::open(
            Arc::clone(plan),
            path,
            StreamSettings {
                inputs: s.inputs,
                frames: s.frames,
                sample_rate: s.sample_rate,
                smoothing_frames: s.smoothing_frames,
                spread: spread_of(s.spread),
            },
        )?;
        *out = Box::into_raw(Box::new(RenderBinauralStream(stream)));
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_process(
    handle: *mut RenderBinauralStream,
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
        let s = unsafe { binaural_stream(handle)? };
        let buffer = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = s.process(buffer, frames)?;
        Ok(())
    })
}

/// `*pending` (set unless the call fails) is whether a pre-roll block remains.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_seek_begin(
    handle: *mut RenderBinauralStream,
    frame: u64,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { binaural_stream(handle)? }.seek_begin(frame)?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_seek_step(
    handle: *mut RenderBinauralStream,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { binaural_stream(handle)? }.seek_step()?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_overrides(
    handle: *mut RenderBinauralStream,
    values: *const RenderLiveOverride,
    values_len: usize,
) -> i32 {
    driver_boundary(ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), || {
        let o = unsafe { live_overrides(values, values_len)? };
        unsafe { binaural_stream(handle)? }.set_overrides(&o)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_pose(
    handle: *mut RenderBinauralStream,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Ok(s) = unsafe { binaural_stream(handle) } {
        s.set_pose([yaw, pitch, roll]);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_stream_destroy(handle: *mut RenderBinauralStream) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

// ── Scene streaming ───────────────────────────────────────────────────────────

/// The Scene stream binaural renderer's configuration; spread as in `RenderBinauralRequest`.
#[repr(C)]
pub struct RenderLiveBinauralRequest {
    sample_rate: u32,
    spread: u32,
    smoothing_frames: u32,
}

/// A Scene stream binaural renderer over a live HRTF state (`mradm_render_binaural_hrtf_live`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_binaural_create(
    hrtf: *const BinauralHrtfHandle,
    request: *const RenderLiveBinauralRequest,
    output: *mut *mut RenderLive,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    boundary(
        message,
        message_len,
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(h), Some(r)) = (
                unsafe { output.as_mut() },
                unsafe { hrtf.as_ref() },
                unsafe { request.as_ref() },
            ) else {
                return Err(bad("无效 Live binaural 参数"));
            };
            *out = ptr::null_mut();
            let renderer = live_binaural::Renderer::new(
                Arc::clone(&h.state),
                &live_binaural::Config {
                    sample_rate: r.sample_rate,
                    spread: r.spread,
                    smoothing_frames: r.smoothing_frames,
                },
            )?;
            *out = Box::into_raw(Box::new(RenderLive::new(LiveRenderer::Binaural(Box::new(
                renderer,
            )))));
            Ok(())
        },
    )
}

#[cfg(test)]
#[path = "render_binaural_test.rs"]
mod tests;
