//! Private EAR boundary. Direct is writable; diffuse and all handles/buffers must not alias.
use crate::{boundary, ptr, slice};
use mradm_dsp::{
    Error, Result,
    ear_post::{FilterBank, Processor},
};
use std::sync::Arc;

fn invalid() -> Error {
    Error::InvalidArgument("Invalid EAR post-processing FFI arguments")
}
unsafe fn input<'a>(p: *const f32, n: usize) -> Result<&'a [f32]> {
    if n > isize::MAX as usize / size_of::<f32>() || (n != 0 && p.is_null()) {
        return Err(invalid());
    }
    Ok(if n == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(p, n) }
    })
}
unsafe fn output<'a>(p: *mut f32, n: usize) -> Result<&'a mut [f32]> {
    if n > isize::MAX as usize / size_of::<f32>() || (n != 0 && p.is_null()) {
        return Err(invalid());
    }
    Ok(if n == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(p, n) }
    })
}
unsafe fn create<T>(out: *mut *mut T, make: impl FnOnce() -> Result<T>) -> Result<()> {
    if out.is_null() {
        return Err(invalid());
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
unsafe fn destroy<T>(p: *mut T) {
    if !p.is_null() {
        unsafe {
            drop(Box::from_raw(p));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ear_filters_create(
    channels: usize,
    firs: *const f32,
    length: usize,
    delay: usize,
    out: *mut *mut Arc<FilterBank>,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            Ok(Arc::new(FilterBank::new(
                channels,
                input(firs, length)?,
                delay,
            )?))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ear_filters_destroy(bank: *mut Arc<FilterBank>) {
    unsafe {
        destroy(bank);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ear_post_create(
    bank: *const Arc<FilterBank>,
    max_frames: usize,
    out: *mut *mut Processor,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            let bank = bank.as_ref().ok_or_else(invalid)?;
            Processor::new(Arc::clone(bank), max_frames)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ear_post_destroy(processor: *mut Processor) {
    unsafe {
        destroy(processor);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ear_post_reset(
    processor: *mut Processor,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        unsafe { processor.as_mut() }.ok_or_else(invalid)?.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_ear_post_process(
    processor: *mut Processor,
    direct: *mut f32,
    direct_len: usize,
    diffuse: *const f32,
    diffuse_len: usize,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        processor.as_mut().ok_or_else(invalid)?.process(
            output(direct, direct_len)?,
            input(diffuse, diffuse_len)?,
            frames,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shared_bank_lifetime_and_parameter_rejections_are_atomic() {
        unsafe {
            let mut firs = [0.; 1024];
            for ch in 0..2 {
                firs[ch * 512] = 1.;
                firs[ch * 512 + 1] = 0.5;
                firs[ch * 512 + 2] = 0.25;
            }
            let mut bank = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_ear_filters_create(
                    2,
                    firs.as_ptr(),
                    1023,
                    255,
                    &mut bank,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert!(bank.is_null());
            assert_eq!(
                mradm_dsp_ear_filters_create(
                    2,
                    ptr::null(),
                    1024,
                    255,
                    &mut bank,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_ear_filters_create(
                    2,
                    firs.as_ptr(),
                    usize::MAX,
                    255,
                    &mut bank,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_ear_filters_create(
                    2,
                    firs.as_ptr(),
                    1024,
                    255,
                    &mut bank,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut first = ptr::dangling_mut();
            let mut second = ptr::null_mut();
            assert_eq!(
                mradm_dsp_ear_post_create(bank, 0, &mut first, ptr::null_mut(), 0),
                1
            );
            assert!(first.is_null());
            assert_eq!(
                mradm_dsp_ear_post_create(bank, usize::MAX, &mut first, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_ear_post_create(bank, 32, &mut first, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_ear_post_create(bank, 32, &mut second, ptr::null_mut(), 0),
                0
            );
            mradm_dsp_ear_filters_destroy(bank);
            let wet = [1.; 2];
            let mut a = [0.; 4];
            let mut b = a;
            for p in [first, second] {
                let mut out = [0.; 2];
                assert_eq!(
                    mradm_dsp_ear_post_process(
                        p,
                        out.as_mut_ptr(),
                        2,
                        wet.as_ptr(),
                        2,
                        1,
                        ptr::null_mut(),
                        0
                    ),
                    0
                );
            }
            a.fill(42.);
            for (direct_len, diffuse_len, frames) in [
                (1, 2, 1),
                (4, 1, 1),
                (4, 2, 3),
                (4, 2, 33),
                (usize::MAX, 2, 1),
                (4, 2, usize::MAX),
            ] {
                assert_eq!(
                    mradm_dsp_ear_post_process(
                        first,
                        a.as_mut_ptr(),
                        direct_len,
                        wet.as_ptr(),
                        diffuse_len,
                        frames,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(a, [42.; 4]);
            }
            assert_eq!(
                mradm_dsp_ear_post_process(
                    first,
                    ptr::null_mut(),
                    2,
                    wet.as_ptr(),
                    2,
                    1,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_ear_post_process(
                    first,
                    a.as_mut_ptr(),
                    2,
                    ptr::null(),
                    2,
                    1,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_ear_post_process(
                    first,
                    ptr::null_mut(),
                    0,
                    ptr::null(),
                    0,
                    0,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            a.fill(0.);
            let zeros = [0.; 2];
            assert_eq!(
                mradm_dsp_ear_post_process(
                    first,
                    a.as_mut_ptr(),
                    4,
                    zeros.as_ptr(),
                    2,
                    1,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                mradm_dsp_ear_post_process(
                    second,
                    b.as_mut_ptr(),
                    4,
                    zeros.as_ptr(),
                    2,
                    1,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(a, b);
            assert!((a[0] - 0.5).abs() < 1e-6);
            assert_eq!(a[2..], [0.; 2]);
            assert_eq!(mradm_dsp_ear_post_reset(first, ptr::null_mut(), 0), 0);
            a.fill(0.);
            assert_eq!(
                mradm_dsp_ear_post_process(
                    first,
                    a.as_mut_ptr(),
                    4,
                    zeros.as_ptr(),
                    2,
                    1,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(a, [0.; 4]);
            mradm_dsp_ear_post_destroy(first);
            mradm_dsp_ear_post_destroy(second);
            mradm_dsp_ear_post_destroy(ptr::null_mut());
            mradm_dsp_ear_filters_destroy(ptr::null_mut());
            assert_eq!(
                mradm_dsp_ear_post_reset(ptr::null_mut(), ptr::null_mut(), 0),
                1
            );
        }
    }
}
