use super::{
    boundary,
    checked::{self, separate},
};
use mradm_dsp::{
    Error,
    vbap::{Panner, PannerSource},
};
use std::{ptr, slice};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_create(
    dirs: *const f32,
    length: usize,
    is_3d: i32,
    output: *mut *mut Panner,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null panner output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        if dirs.is_null() || !(4..=200_000).contains(&length) {
            return Err(Error::InvalidArgument("Invalid speaker array"));
        }
        let panner = Panner::new(unsafe { slice::from_raw_parts(dirs, length) }, is_3d != 0)?;
        unsafe {
            *output = Box::into_raw(Box::new(panner));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_destroy(handle: *mut Panner) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_gains(
    handle: *const Panner,
    az: f32,
    el: f32,
    spread: f32,
    output: *mut f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || output.is_null() {
            return Err(Error::InvalidArgument("Null panner argument"));
        }
        let panner = unsafe { &*handle };
        if length != panner.real_speakers {
            return Err(Error::InvalidArgument("Invalid gain buffer length"));
        }
        panner.gains(az, el, spread, unsafe {
            slice::from_raw_parts_mut(output, length)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_sources(
    handle: *const Panner,
    sources: *const PannerSource,
    count: usize,
    spread: u32,
    sum: u32,
    scratch: *mut f32,
    output: *mut f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || spread > 1 || sum > 1 {
            return Err(Error::InvalidArgument("Invalid panner source query"));
        }
        separate(scratch, length, output, length)?;
        separate(sources, count, scratch, length)?;
        separate(sources, count, output, length)?;
        let panner = unsafe { &*handle };
        let (sources, scratch, output) = unsafe {
            (
                checked::input(sources, count)?,
                checked::output(scratch, length)?,
                checked::output(output, length)?,
            )
        };
        panner.source_gains(sources, spread != 0, sum != 0, scratch, output)
    })
}
