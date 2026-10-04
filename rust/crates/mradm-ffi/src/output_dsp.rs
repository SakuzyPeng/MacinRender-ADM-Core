//! Private buffer-oriented boundary for live gains and device peak protection.
use crate::{boundary, ptr, slice};
use mradm_dsp::{Error, Result, gain::GainBank, peak_guard::StereoPeakGuard};

unsafe fn input<'a>(data: *const f32, length: usize) -> Result<&'a [f32]> {
    if length > isize::MAX as usize / size_of::<f32>() || (length != 0 && data.is_null()) {
        return Err(Error::InvalidArgument("Invalid output DSP input buffer"));
    }
    Ok(if length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, length) }
    })
}
unsafe fn output<'a>(data: *mut f32, length: usize) -> Result<&'a mut [f32]> {
    if length > isize::MAX as usize / size_of::<f32>() || (length != 0 && data.is_null()) {
        return Err(Error::InvalidArgument("Invalid output DSP output buffer"));
    }
    Ok(if length == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(data, length) }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_gain_create(
    channels: usize,
    rate: u32,
    ramp_ms: u32,
    result: *mut *mut GainBank,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if result.is_null() {
            return Err(Error::InvalidArgument("Null gain output handle"));
        }
        unsafe {
            *result = ptr::null_mut();
        }
        let bank = GainBank::new(channels, rate, ramp_ms)?;
        unsafe {
            *result = Box::into_raw(Box::new(bank));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_gain_destroy(handle: *mut GainBank) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_gain_reset(
    handle: *mut GainBank,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let bank = unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null gain handle"))?;
        bank.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_gain_set_targets(
    handle: *mut GainBank,
    targets: *const f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let bank = unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null gain handle"))?;
        bank.set_targets(unsafe { input(targets, length) }?)
    })
}
/// mode 0 generates interleaved envelopes; mode 1 multiplies interleaved PCM in place.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_gain_process(
    handle: *mut GainBank,
    data: *mut f32,
    length: usize,
    mode: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let bank = unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null gain handle"))?;
        if mode > 1 {
            return Err(Error::InvalidArgument("Invalid gain process mode"));
        }
        let samples = unsafe { output(data, length) }?;
        if mode == 0 {
            bank.fill(samples)
        } else {
            bank.apply(samples)
        }
    })
}

#[repr(C)]
#[derive(Default, Debug, PartialEq)]
pub struct PeakStatus {
    lookahead: usize,
    buffered: usize,
    writable: usize,
    readable: usize,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_guard_create(
    rate: u32,
    result: *mut *mut StereoPeakGuard,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if result.is_null() {
            return Err(Error::InvalidArgument("Null peak guard output handle"));
        }
        unsafe {
            *result = ptr::null_mut();
        }
        let guard = StereoPeakGuard::new(rate)?;
        unsafe {
            *result = Box::into_raw(Box::new(guard));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_guard_destroy(handle: *mut StereoPeakGuard) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_guard_reset(
    handle: *mut StereoPeakGuard,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let guard =
            unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null peak guard handle"))?;
        guard.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_guard_status(
    handle: *const StereoPeakGuard,
    ended: u32,
    status: *mut PeakStatus,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let guard =
            unsafe { handle.as_ref() }.ok_or(Error::InvalidArgument("Null peak guard handle"))?;
        if ended > 1 || status.is_null() {
            return Err(Error::InvalidArgument(
                "Invalid peak guard status arguments",
            ));
        }
        unsafe {
            *status = PeakStatus {
                lookahead: guard.lookahead_frames(),
                buffered: guard.buffered_frames(),
                writable: guard.writable_frames(),
                readable: guard.readable_frames(ended != 0),
            };
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_guard_push(
    handle: *mut StereoPeakGuard,
    data: *const f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let guard =
            unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null peak guard handle"))?;
        guard.push(unsafe { input(data, length) }?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_guard_pop(
    handle: *mut StereoPeakGuard,
    data: *mut f32,
    length: usize,
    volume: f32,
    ended: u32,
    frames: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let guard =
            unsafe { handle.as_mut() }.ok_or(Error::InvalidArgument("Null peak guard handle"))?;
        if ended > 1 || frames.is_null() {
            return Err(Error::InvalidArgument("Invalid peak guard pop arguments"));
        }
        let count = guard.pop(unsafe { output(data, length) }?, volume, ended != 0)?;
        unsafe {
            *frames = count;
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gain_rejections_preserve_targets_buffers_and_history() {
        unsafe {
            let mut bank = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_gain_create(0, 48000, 20, &mut bank, ptr::null_mut(), 0),
                1
            );
            assert!(bank.is_null());
            assert_eq!(
                mradm_dsp_gain_create(2, 48000, 20, &mut bank, ptr::null_mut(), 0),
                0
            );
            let targets = [0.25, 0.5];
            assert_eq!(
                mradm_dsp_gain_set_targets(bank, targets.as_ptr(), 2, ptr::null_mut(), 0),
                0
            );
            let bad = [0.75, f32::NAN];
            assert_eq!(
                mradm_dsp_gain_set_targets(bank, bad.as_ptr(), 2, ptr::null_mut(), 0),
                1
            );
            let mut pcm = [42.; 4];
            for mode in [0, 1] {
                assert_eq!(
                    mradm_dsp_gain_process(bank, pcm.as_mut_ptr(), 3, mode, ptr::null_mut(), 0),
                    1
                );
                assert_eq!(
                    mradm_dsp_gain_process(bank, ptr::null_mut(), 2, mode, ptr::null_mut(), 0),
                    1
                );
                assert_eq!(
                    mradm_dsp_gain_process(
                        bank,
                        pcm.as_mut_ptr(),
                        usize::MAX,
                        mode,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
            }
            assert_eq!(
                mradm_dsp_gain_process(bank, pcm.as_mut_ptr(), 4, 2, ptr::null_mut(), 0),
                1
            );
            assert_eq!(pcm, [42.; 4]);
            assert_eq!(
                mradm_dsp_gain_process(bank, ptr::null_mut(), 0, 0, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_gain_process(bank, pcm.as_mut_ptr(), 4, 0, ptr::null_mut(), 0),
                0
            );
            assert_eq!(pcm, [0.25, 0.5, 0.25, 0.5]);
            assert_eq!(mradm_dsp_gain_reset(bank, ptr::null_mut(), 0), 0);
            assert_eq!(
                mradm_dsp_gain_process(bank, pcm.as_mut_ptr(), 4, 0, ptr::null_mut(), 0),
                0
            );
            assert_eq!(pcm, [1.; 4]);
            mradm_dsp_gain_destroy(bank);
            mradm_dsp_gain_destroy(ptr::null_mut());
            assert_eq!(mradm_dsp_gain_reset(ptr::null_mut(), ptr::null_mut(), 0), 1);
        }
    }

    #[test]
    fn peak_rejections_preserve_audio_counters_and_status() {
        unsafe {
            let mut guard = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_peak_guard_create(0, &mut guard, ptr::null_mut(), 0),
                1
            );
            assert!(guard.is_null());
            assert_eq!(
                mradm_dsp_peak_guard_create(48000, &mut guard, ptr::null_mut(), 0),
                0
            );
            let input = [0.125, -0.25];
            assert_eq!(
                mradm_dsp_peak_guard_push(guard, input.as_ptr(), 2, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_peak_guard_push(guard, input.as_ptr(), 1, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_peak_guard_push(guard, ptr::null(), 2, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_peak_guard_push(guard, input.as_ptr(), usize::MAX, ptr::null_mut(), 0),
                1
            );
            let mut pcm = [42.; 4];
            let mut frames = 123;
            for (length, volume, ended) in [
                (3, 1., 1),
                (4, f32::NAN, 1),
                (4, -1., 1),
                (4, 1.01, 1),
                (4, 1., 2),
                (usize::MAX, 1., 1),
            ] {
                assert_eq!(
                    mradm_dsp_peak_guard_pop(
                        guard,
                        pcm.as_mut_ptr(),
                        length,
                        volume,
                        ended,
                        &mut frames,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(pcm, [42.; 4]);
                assert_eq!(frames, 123);
            }
            assert_eq!(
                mradm_dsp_peak_guard_pop(
                    guard,
                    pcm.as_mut_ptr(),
                    4,
                    1.,
                    1,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            let mut state = PeakStatus::default();
            assert_eq!(
                mradm_dsp_peak_guard_status(guard, 2, &mut state, ptr::null_mut(), 0),
                1
            );
            assert_eq!(state, PeakStatus::default());
            assert_eq!(
                mradm_dsp_peak_guard_status(guard, 1, &mut state, ptr::null_mut(), 0),
                0
            );
            assert_eq!(state.buffered, 1);
            assert_eq!(state.readable, 1);
            assert_eq!(
                mradm_dsp_peak_guard_pop(
                    guard,
                    pcm.as_mut_ptr(),
                    4,
                    0.5,
                    1,
                    &mut frames,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(frames, 1);
            assert_eq!(pcm, [0.0625, -0.125, 42., 42.]);
            assert_eq!(mradm_dsp_peak_guard_reset(guard, ptr::null_mut(), 0), 0);
            mradm_dsp_peak_guard_destroy(guard);
            mradm_dsp_peak_guard_destroy(ptr::null_mut());
            assert_eq!(
                mradm_dsp_peak_guard_status(ptr::null(), 0, &mut state, ptr::null_mut(), 0),
                1
            );
        }
    }
}
