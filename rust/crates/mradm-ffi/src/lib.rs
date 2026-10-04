//! Private C boundary. Pointers must be valid, aligned, disjoint and live for the
//! call. A mutable handle may only be used by one thread at a time. Each handle
//! is destroyed by its matching Rust destructor, never by the caller's allocator.
#![allow(clippy::missing_safety_doc)]

use mradm_dsp::{Error, Result, fft::RealFft};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
};
mod convolution;
mod hptf;
mod hrtf;
mod meter;
mod output_dsp;
mod resampler;
mod spatial;
mod spreader;

/// Error codes follow the project's public ErrorCode values. Messages belong to
/// the caller, making error reporting reentrant without thread-local allocation.
fn boundary(message: *mut u8, capacity: usize, f: impl FnOnce() -> Result<()>) -> i32 {
    let result = catch_unwind(AssertUnwindSafe(f));
    let (code, detail) = match result {
        Ok(Ok(())) => (0, ""),
        Ok(Err(Error::InvalidArgument(s))) => (1, s),
        Ok(Err(Error::Unsupported(s))) => (2, s),
        Ok(Err(Error::Io(s))) => (3, s),
        Ok(Err(Error::RenderFailed(s))) => (4, s),
        Err(_) => (6, "Rust DSP panic"),
    };
    if !message.is_null() && capacity != 0 {
        let mut length = detail.len().min(capacity - 1);
        while !detail.is_char_boundary(length) {
            length -= 1;
        }
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), message, length);
            *message.add(length) = 0;
        }
    }
    code
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_fft_create(
    length: usize,
    output: *mut *mut RealFft,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null FFT output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let plan = RealFft::new(length)?;
        unsafe {
            *output = Box::into_raw(Box::new(plan));
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_fft_destroy(handle: *mut RealFft) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_fft_forward(
    handle: *mut RealFft,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || input.is_null() || output.is_null() {
            return Err(Error::InvalidArgument("Null FFT argument"));
        }
        let plan = unsafe { &mut *handle };
        if input_len != plan.len() || output_len != 2 * plan.bins() {
            return Err(Error::InvalidArgument("FFT buffer length mismatch"));
        }
        plan.forward_interleaved(unsafe { slice::from_raw_parts(input, input_len) }, unsafe {
            slice::from_raw_parts_mut(output, output_len)
        })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_fft_inverse(
    handle: *mut RealFft,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || input.is_null() || output.is_null() {
            return Err(Error::InvalidArgument("Null FFT argument"));
        }
        let plan = unsafe { &mut *handle };
        if input_len != 2 * plan.bins() || output_len != plan.len() {
            return Err(Error::InvalidArgument("Inverse FFT buffer length mismatch"));
        }
        plan.inverse_interleaved(unsafe { slice::from_raw_parts(input, input_len) }, unsafe {
            slice::from_raw_parts_mut(output, output_len)
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn invalid_arguments_are_errors_and_failed_create_clears_output() {
        let mut handle = ptr::dangling_mut();
        let mut message = [0u8; 128];
        unsafe {
            assert_eq!(
                mradm_dsp_fft_create(3, &mut handle, message.as_mut_ptr(), message.len()),
                1
            );
            assert!(handle.is_null());
            assert_ne!(message[0], 0);
            assert_eq!(
                mradm_dsp_fft_forward(
                    handle,
                    ptr::null(),
                    0,
                    ptr::null_mut(),
                    0,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            mradm_dsp_fft_destroy(handle);
        }
    }
    #[test]
    fn panics_do_not_cross_the_boundary() {
        assert_eq!(boundary(ptr::null_mut(), 0, || panic!("test panic")), 6);
    }

    #[test]
    fn truncated_error_buffers_preserve_utf8_and_termination() {
        let detail = "HpTF:系数无效";
        for capacity in 1..24 {
            let mut message = [42u8; 24];
            assert_eq!(
                boundary(message.as_mut_ptr(), capacity, || Err(
                    Error::InvalidArgument(detail)
                )),
                1
            );
            let end = message[..capacity].iter().position(|&b| b == 0).unwrap();
            let prefix = std::str::from_utf8(&message[..end]).unwrap();
            assert!(detail.starts_with(prefix));
            assert!(message[capacity..].iter().all(|&b| b == 42));
        }
    }
}
