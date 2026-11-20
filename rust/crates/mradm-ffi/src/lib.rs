//! Private C boundary. Pointers must be valid, aligned, disjoint and live for the
//! call. A mutable handle may only be used by one thread at a time. Each handle
//! is destroyed by its matching Rust destructor, never by the caller's allocator.
//!
//! The `mradm_*` entries serve the C++ islands and tests through one table of function pointers
//! (`services`, never by name since 6.6 A); Rust callers use only [`api`].

// The islands and C libraries this crate calls by name: a crate nothing names is not linked.
extern crate mradm_islands_sys;

use mradm_dsp::{Error, Result};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
};
mod adm;
pub mod api;
mod checked;
mod engine;
mod engine_render;
mod engine_service;
mod flac;
mod hptf;
mod metadata_temp;
mod meter;
mod opus_mka;
mod osc;
mod output_dsp;
mod render;
mod render_apple;
mod room;
mod room_cuda_selection;
mod room_environment;
mod scene;
mod scene_math;
#[cfg(windows)]
mod scene_worker;
mod services;
mod spatial;
#[cfg(test)]
mod test_alloc;
mod wav;

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

#[cfg(test)]
mod tests {
    use super::*;
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
