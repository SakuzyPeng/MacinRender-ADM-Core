use super::boundary;
use mradm_dsp::{
    Error,
    filterbank::{DELAY, FRAME},
    spreader::Spreader,
};
use std::{ptr, slice};

pub struct Handle {
    processor: Spreader,
    input: Vec<f32>,
    output: Vec<f32>,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_spreader_create(
    ir: *const f32,
    ir_length: usize,
    dirs: *const f32,
    dir_length: usize,
    taps: usize,
    rate: u32,
    seeds: *const u64,
    count: usize,
    output: *mut *mut Handle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null spreader output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        if taps == 0
            || dir_length > 200_000
            || ir.is_null()
            || dirs.is_null()
            || seeds.is_null()
            || count == 0
            || count > 8
            || dir_length < 8
            || !dir_length.is_multiple_of(2)
            || dir_length.checked_mul(taps) != Some(ir_length)
            || ir_length > isize::MAX as usize / 4
        {
            return Err(Error::InvalidArgument(
                "Invalid spreader creation arguments",
            ));
        }
        let processor = Spreader::new(
            unsafe { slice::from_raw_parts(ir, ir_length) },
            unsafe { slice::from_raw_parts(dirs, dir_length) },
            taps,
            rate,
            unsafe { slice::from_raw_parts(seeds, count) },
        )?;
        let handle = Handle {
            processor,
            input: vec![0.; count * FRAME],
            output: vec![0.; 2 * FRAME],
        };
        unsafe {
            *output = Box::into_raw(Box::new(handle));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_spreader_destroy(handle: *mut Handle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_spreader_set_source(
    handle: *mut Handle,
    index: usize,
    az: f32,
    el: f32,
    spread: f32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() {
            return Err(Error::InvalidArgument("Null spreader handle"));
        }
        unsafe { &mut *handle }
            .processor
            .set_source(index, az, el, spread)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_spreader_process(
    handle: *mut Handle,
    inputs: *const *const f32,
    count: usize,
    left: *mut f32,
    right: *mut f32,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null()
            || inputs.is_null()
            || left.is_null()
            || right.is_null()
            || frames != FRAME
        {
            return Err(Error::InvalidArgument("Invalid spreader process arguments"));
        }
        let state = unsafe { &mut *handle };
        if count != state.processor.sources() {
            return Err(Error::InvalidArgument("Spreader channel count mismatch"));
        }
        let inputs = unsafe { slice::from_raw_parts(inputs, count) };
        for (channel, &source) in inputs.iter().enumerate() {
            if source.is_null() {
                return Err(Error::InvalidArgument("Null spreader input channel"));
            }
            state.input[channel * FRAME..(channel + 1) * FRAME]
                .copy_from_slice(unsafe { slice::from_raw_parts(source, FRAME) });
        }
        state.processor.process(&state.input, &mut state.output)?;
        unsafe {
            ptr::copy_nonoverlapping(state.output.as_ptr(), left, FRAME);
            ptr::copy_nonoverlapping(state.output.as_ptr().add(FRAME), right, FRAME);
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn mradm_dsp_spreader_delay() -> u32 {
    DELAY as u32
}
