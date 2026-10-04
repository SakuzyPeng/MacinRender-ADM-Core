use crate::{boundary, ptr, slice};
use mradm_dsp::{Error, resampler::Resampler};

#[repr(C)]
pub struct Progress {
    input_frames: usize,
    output_frames: usize,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_resampler_create(
    channels: usize,
    input_rate: u32,
    output_rate: u32,
    output: *mut *mut Resampler,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null resampler output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let resampler = Resampler::new(channels, input_rate, output_rate)?;
        unsafe {
            *output = Box::into_raw(Box::new(resampler));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_resampler_destroy(handle: *mut Resampler) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_resampler_reset(
    handle: *mut Resampler,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let resampler =
            unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null resampler handle"))?;
        resampler.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_resampler_process(
    handle: *mut Resampler,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
    end: u32,
    progress: *mut Progress,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let resampler =
            unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null resampler handle"))?;
        if (input_len != 0 && input.is_null())
            || output.is_null()
            || progress.is_null()
            || input_len > isize::MAX as usize / size_of::<f32>()
            || output_len > isize::MAX as usize / size_of::<f32>()
            || !input_len.is_multiple_of(resampler.channels())
            || !output_len.is_multiple_of(resampler.channels())
            || end > 1
            || (end != 0 && input_len != 0)
        {
            return Err(Error::InvalidArgument(
                "Invalid resampler buffers or end flag",
            ));
        }
        let input = if input_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(input, input_len) }
        };
        let output = unsafe { slice::from_raw_parts_mut(output, output_len) };
        let result = if end == 0 {
            let p = resampler.process(input, output)?;
            Progress {
                input_frames: p.input_frames,
                output_frames: p.output_frames,
            }
        } else {
            Progress {
                input_frames: 0,
                output_frames: resampler.finish(output)?,
            }
        };
        unsafe {
            *progress = result;
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn invalid_sizes_end_flags_and_nulls_do_not_write_progress() {
        unsafe {
            let mut handle = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_resampler_create(2, 0, 44100, &mut handle, ptr::null_mut(), 0),
                1
            );
            assert!(handle.is_null());
            assert_eq!(
                mradm_dsp_resampler_create(2, 48000, 44100, &mut handle, ptr::null_mut(), 0),
                0
            );
            let input = [0.2; 4];
            let mut output = [42.0; 8];
            let mut progress = Progress {
                input_frames: 9,
                output_frames: 9,
            };
            for (n, m, end) in [
                (usize::MAX, 8, 0),
                (4, usize::MAX, 0),
                (3, 8, 0),
                (4, 7, 0),
                (4, 8, 2),
                (4, 8, 1),
            ] {
                assert_eq!(
                    mradm_dsp_resampler_process(
                        handle,
                        input.as_ptr(),
                        n,
                        output.as_mut_ptr(),
                        m,
                        end,
                        &mut progress,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!((progress.input_frames, progress.output_frames), (9, 9));
                assert_eq!(output, [42.0; 8]);
            }
            assert_eq!(
                mradm_dsp_resampler_process(
                    handle,
                    ptr::null(),
                    4,
                    output.as_mut_ptr(),
                    8,
                    0,
                    &mut progress,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_resampler_process(
                    handle,
                    input.as_ptr(),
                    4,
                    ptr::null_mut(),
                    8,
                    0,
                    &mut progress,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_resampler_process(
                    handle,
                    input.as_ptr(),
                    4,
                    output.as_mut_ptr(),
                    8,
                    0,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_resampler_process(
                    handle,
                    input.as_ptr(),
                    4,
                    output.as_mut_ptr(),
                    8,
                    0,
                    &mut progress,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(progress.input_frames, 2);
            assert_eq!(
                mradm_dsp_resampler_process(
                    handle,
                    ptr::null(),
                    0,
                    output.as_mut_ptr(),
                    8,
                    1,
                    &mut progress,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(progress.output_frames, 2);
            assert_eq!(
                mradm_dsp_resampler_process(
                    handle,
                    ptr::null(),
                    0,
                    output.as_mut_ptr(),
                    8,
                    1,
                    &mut progress,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(progress.output_frames, 0);
            assert_eq!(mradm_dsp_resampler_reset(handle, ptr::null_mut(), 0), 0);
            mradm_dsp_resampler_destroy(handle);
            mradm_dsp_resampler_destroy(ptr::null_mut());
        }
    }
}
