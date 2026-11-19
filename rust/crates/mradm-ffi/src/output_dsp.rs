//! Private buffer-oriented boundary for live gains.
use crate::{boundary, ptr, slice};
use mradm_dsp::{Error, Result, gain::GainBank};

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

pub(crate) unsafe extern "C" fn mradm_dsp_gain_create(
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
pub(crate) unsafe extern "C" fn mradm_dsp_gain_destroy(handle: *mut GainBank) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
pub(crate) unsafe extern "C" fn mradm_dsp_gain_reset(
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
pub(crate) unsafe extern "C" fn mradm_dsp_gain_set_targets(
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
pub(crate) unsafe extern "C" fn mradm_dsp_gain_process(
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
}
