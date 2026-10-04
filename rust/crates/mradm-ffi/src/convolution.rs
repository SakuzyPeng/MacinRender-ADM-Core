use crate::{boundary, ptr, slice};
use mradm_dsp::{
    Error, Result,
    convolution::{LiveConvolver, LiveState, OlaConvolver},
    diffuse::DiffuseDelay,
};

// Callers guarantee validity, alignment and non-aliasing. Empty buffers may be
// null; reject overflowing lengths before constructing any Rust slice.
unsafe fn input<'a>(data: *const f32, length: usize) -> Result<&'a [f32]> {
    if length > isize::MAX as usize / size_of::<f32>() || (length != 0 && data.is_null()) {
        return Err(Error::InvalidArgument("Invalid DSP input buffer"));
    }
    Ok(if length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, length) }
    })
}
unsafe fn output<'a>(data: *mut f32, length: usize) -> Result<&'a mut [f32]> {
    if length > isize::MAX as usize / size_of::<f32>() || (length != 0 && data.is_null()) {
        return Err(Error::InvalidArgument("Invalid DSP output buffer"));
    }
    Ok(if length == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(data, length) }
    })
}
unsafe fn handle<'a, T>(value: *mut T) -> Result<&'a mut T> {
    unsafe { value.as_mut() }.ok_or(Error::InvalidArgument("Null DSP handle"))
}
unsafe fn create<T>(out: *mut *mut T, make: impl FnOnce() -> Result<T>) -> Result<()> {
    if out.is_null() {
        return Err(Error::InvalidArgument("Null DSP output handle"));
    }
    unsafe {
        *out = ptr::null_mut();
    }
    let value = make()?;
    unsafe {
        *out = Box::into_raw(Box::new(value));
    }
    Ok(())
}
unsafe fn destroy<T>(value: *mut T) {
    if !value.is_null() {
        unsafe {
            drop(Box::from_raw(value));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_convolver_create(
    hrtf_length: usize,
    maximum_frames: usize,
    sample_rate: u32,
    out: *mut *mut LiveConvolver,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            LiveConvolver::new(hrtf_length, maximum_frames, sample_rate)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_convolver_destroy(value: *mut LiveConvolver) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_state_create(
    value: *const LiveConvolver,
    out: *mut *mut LiveState,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            let convolver = value
                .as_ref()
                .ok_or(Error::InvalidArgument("Null live convolver"))?;
            Ok(convolver.make_state())
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_state_destroy(value: *mut LiveState) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_state_reset(
    value: *mut LiveState,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        unsafe { handle(value)? }.reset();
        Ok(())
    })
}
#[repr(C)]
pub struct LiveInfo {
    initialized: u32,
    tail_remaining: u32,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_state_info(
    value: *const LiveState,
    out: *mut LiveInfo,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let state = unsafe { value.as_ref() }.ok_or(Error::InvalidArgument("Null live state"))?;
        if out.is_null() {
            return Err(Error::InvalidArgument("Null live state info"));
        }
        unsafe {
            *out = LiveInfo {
                initialized: u32::from(state.initialized()),
                tail_remaining: state.tail_remaining() as u32,
            };
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_initialize(
    value: *mut LiveConvolver,
    state: *mut LiveState,
    hrtf: *const f32,
    hrtf_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let convolver = handle(value)?;
        if hrtf_len != convolver.spectrum_len() {
            return Err(Error::InvalidArgument("Invalid live spectrum length"));
        }
        convolver.initialize(handle(state)?, input(hrtf, hrtf_len)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_process(
    value: *mut LiveConvolver,
    state: *mut LiveState,
    hrtf: *const f32,
    hrtf_len: usize,
    samples: *const f32,
    frames: usize,
    left: *mut f32,
    left_len: usize,
    right: *mut f32,
    right_len: usize,
    follows_ramp: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let convolver = handle(value)?;
        if frames > convolver.maximum_frames()
            || left_len != frames
            || right_len != frames
            || hrtf_len != convolver.spectrum_len()
            || follows_ramp > 1
        {
            return Err(Error::InvalidArgument(
                "Invalid live convolution buffers or ramp flag",
            ));
        }
        convolver.process(
            handle(state)?,
            input(hrtf, hrtf_len)?,
            input(samples, frames)?,
            output(left, left_len)?,
            output(right, right_len)?,
            follows_ramp != 0,
        )
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ola_create(
    fft_length: usize,
    overlap: usize,
    maximum_frames: usize,
    out: *mut *mut OlaConvolver,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            OlaConvolver::new(fft_length, overlap, maximum_frames)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ola_destroy(value: *mut OlaConvolver) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ola_reset(
    value: *mut OlaConvolver,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        unsafe { handle(value)? }.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ola_process(
    value: *mut OlaConvolver,
    samples: *const f32,
    frames: usize,
    start_hrtf: *const f32,
    start_len: usize,
    start_gain: f32,
    end_hrtf: *const f32,
    end_len: usize,
    end_gain: f32,
    left: *mut f32,
    left_len: usize,
    right: *mut f32,
    right_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let convolver = handle(value)?;
        if frames > convolver.maximum_frames()
            || left_len != frames
            || right_len != frames
            || start_len != convolver.spectrum_len()
            || if end_hrtf.is_null() {
                end_len != 0
            } else {
                end_len != convolver.spectrum_len()
            }
        {
            return Err(Error::InvalidArgument("Invalid OLA convolution buffers"));
        }
        let end = if end_hrtf.is_null() {
            None
        } else {
            Some((input(end_hrtf, end_len)?, end_gain))
        };
        convolver.process(
            input(samples, frames)?,
            input(start_hrtf, start_len)?,
            start_gain,
            end,
            output(left, left_len)?,
            output(right, right_len)?,
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ola_silence(
    value: *mut OlaConvolver,
    left: *mut f32,
    left_len: usize,
    right: *mut f32,
    right_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let convolver = handle(value)?;
        if left_len != right_len || left_len > convolver.maximum_frames() {
            return Err(Error::InvalidArgument("Invalid OLA silence buffers"));
        }
        convolver.advance_silence(output(left, left_len)?, output(right, right_len)?)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_diffuse_create(
    out: *mut *mut DiffuseDelay,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(DiffuseDelay::default()))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_diffuse_destroy(value: *mut DiffuseDelay) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_diffuse_reset(
    value: *mut DiffuseDelay,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        unsafe { handle(value)? }.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_diffuse_process(
    value: *mut DiffuseDelay,
    samples: *const f32,
    frames: usize,
    out: *mut f32,
    out_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if frames != out_len {
            return Err(Error::InvalidArgument("Invalid diffuse buffer lengths"));
        }
        handle(value)?.process(input(samples, frames)?, output(out, out_len)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_diffuse_mix(
    value: *mut DiffuseDelay,
    samples: *mut f32,
    frames: usize,
    start_gain: f32,
    end_gain: f32,
    start_diffuse: f32,
    end_diffuse: f32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        handle(value)?.mix(
            output(samples, frames)?,
            [start_gain, end_gain],
            [start_diffuse, end_diffuse],
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn live_boundary_checks_sizes_flags_and_state_lifetime() {
        unsafe {
            let mut convolver = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_live_convolver_create(
                    64,
                    usize::MAX,
                    48000,
                    &mut convolver,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert!(convolver.is_null());
            assert_eq!(
                mradm_dsp_live_convolver_create(64, 32, 48000, &mut convolver, ptr::null_mut(), 0),
                0
            );
            let mut state = ptr::null_mut();
            assert_eq!(
                mradm_dsp_live_state_create(convolver, &mut state, ptr::null_mut(), 0),
                0
            );
            let mut spectrum = [0.0; 132];
            for bin in spectrum.as_chunks_mut::<4>().0 {
                bin[0] = 1.0;
                bin[2] = 0.5;
            }
            let samples = [0.25; 32];
            let (mut left, mut right) = ([42.0; 32], [43.0; 32]);
            for (n, l, r, h, ramp) in [
                (usize::MAX, 32, 32, 132, 0),
                (32, 31, 32, 132, 0),
                (32, 32, 31, 132, 0),
                (32, 32, 32, usize::MAX, 0),
                (32, 32, 32, 132, 2),
            ] {
                assert_eq!(
                    mradm_dsp_live_process(
                        convolver,
                        state,
                        spectrum.as_ptr(),
                        h,
                        samples.as_ptr(),
                        n,
                        left.as_mut_ptr(),
                        l,
                        right.as_mut_ptr(),
                        r,
                        ramp,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
            }
            assert_eq!((left, right), ([42.0; 32], [43.0; 32]));
            assert_eq!(
                mradm_dsp_live_process(
                    convolver,
                    state,
                    spectrum.as_ptr(),
                    132,
                    ptr::null(),
                    0,
                    ptr::null_mut(),
                    0,
                    ptr::null_mut(),
                    0,
                    0,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut info = LiveInfo {
                initialized: 7,
                tail_remaining: 7,
            };
            assert_eq!(
                mradm_dsp_live_state_info(state, &mut info, ptr::null_mut(), 0),
                0
            );
            assert_eq!((info.initialized, info.tail_remaining), (0, 0));
            assert_eq!(
                mradm_dsp_live_process(
                    convolver,
                    state,
                    spectrum.as_ptr(),
                    132,
                    samples.as_ptr(),
                    32,
                    left.as_mut_ptr(),
                    32,
                    right.as_mut_ptr(),
                    32,
                    0,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            for (l, r) in left.into_iter().zip(right) {
                assert!((l - 0.25).abs() < 1e-6 && (r - 0.125).abs() < 1e-6);
            }
            // States own all storage and do not borrow the workspace.
            mradm_dsp_live_convolver_destroy(convolver);
            assert_eq!(
                mradm_dsp_live_state_info(state, &mut info, ptr::null_mut(), 0),
                0
            );
            assert_eq!((info.initialized, info.tail_remaining), (1, 63));
            assert_eq!(mradm_dsp_live_state_reset(state, ptr::null_mut(), 0), 0);
            mradm_dsp_live_state_destroy(state);
            mradm_dsp_live_state_destroy(ptr::null_mut());
            mradm_dsp_live_convolver_destroy(ptr::null_mut());
        }
    }

    #[test]
    fn ola_and_diffuse_boundary_reject_invalid_buffers_without_writes() {
        unsafe {
            let mut ola = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_ola_create(64, 64, 32, &mut ola, ptr::null_mut(), 0),
                1
            );
            assert!(ola.is_null());
            assert_eq!(
                mradm_dsp_ola_create(64, 31, 32, &mut ola, ptr::null_mut(), 0),
                0
            );
            let hrtf = [0.0; 132];
            let input = [0.0; 32];
            let (mut left, mut right) = ([42.0; 32], [43.0; 32]);
            for (end, length) in [
                (ptr::null(), 132),
                (hrtf.as_ptr(), 0),
                (hrtf.as_ptr(), usize::MAX),
            ] {
                assert_eq!(
                    mradm_dsp_ola_process(
                        ola,
                        input.as_ptr(),
                        32,
                        hrtf.as_ptr(),
                        132,
                        1.0,
                        end,
                        length,
                        1.0,
                        left.as_mut_ptr(),
                        32,
                        right.as_mut_ptr(),
                        32,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
            }
            assert_eq!(
                mradm_dsp_ola_silence(
                    ola,
                    left.as_mut_ptr(),
                    32,
                    right.as_mut_ptr(),
                    31,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!((left, right), ([42.0; 32], [43.0; 32]));
            assert_eq!(
                mradm_dsp_ola_process(
                    ola,
                    ptr::null(),
                    0,
                    hrtf.as_ptr(),
                    132,
                    1.0,
                    ptr::null(),
                    0,
                    1.0,
                    ptr::null_mut(),
                    0,
                    ptr::null_mut(),
                    0,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            mradm_dsp_ola_destroy(ola);
            mradm_dsp_ola_destroy(ptr::null_mut());
            let mut diffuse = ptr::null_mut();
            assert_eq!(
                mradm_dsp_diffuse_create(&mut diffuse, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_diffuse_process(
                    diffuse,
                    ptr::null(),
                    32,
                    left.as_mut_ptr(),
                    32,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_diffuse_mix(
                    diffuse,
                    left.as_mut_ptr(),
                    usize::MAX,
                    1.0,
                    1.0,
                    0.0,
                    0.0,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_diffuse_mix(
                    diffuse,
                    left.as_mut_ptr(),
                    32,
                    f32::NAN,
                    1.0,
                    0.0,
                    0.0,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(left, [42.0; 32]);
            assert_eq!(
                mradm_dsp_diffuse_mix(
                    diffuse,
                    ptr::null_mut(),
                    0,
                    1.0,
                    1.0,
                    0.0,
                    0.0,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(mradm_dsp_diffuse_reset(diffuse, ptr::null_mut(), 0), 0);
            mradm_dsp_diffuse_destroy(diffuse);
            mradm_dsp_diffuse_destroy(ptr::null_mut());
        }
    }
}
