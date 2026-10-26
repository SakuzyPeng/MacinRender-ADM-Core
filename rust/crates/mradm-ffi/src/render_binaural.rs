//! The binaural HRTF state (PCM consistency 4.7 A): loading, preparation and the Scene stream
//! cache run in Rust `mradm-render::binaural_hrtf`; the C++ renderers borrow its view.
use super::*;
use mradm_dsp::hrtf_filters::Filters;
use mradm_render::binaural_hrtf::{self, Sofa, State};
use std::path::Path;
use std::sync::Arc;

/// A prepared HRTF state.
pub struct BinauralHrtfHandle {
    _state: Arc<State>,
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
    *out = Box::into_raw(Box::new(BinauralHrtfHandle { _state: state }));
    Ok(())
}

/// The file renderer's HRTF state: the built-in set (`sofa` 0) or a SOFA file at `input_rate`,
/// prepared for `block`-frame OLA blocks. The view lives as long as the handle.
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
