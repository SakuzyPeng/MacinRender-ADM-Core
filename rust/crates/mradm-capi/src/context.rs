//! `adm_context_t`: the caller's handle for the render, inspect, preview and monitor entries, holding
//! the message of the last failed call (`adm_context_last_error_message`). A context is used by one
//! thread at a time, as before.
//!
//! The exports still in `src/adm_c_api/adm_c_api.cpp` set the message through
//! `mradm_capi_context_set_error` (`src/adm_c_api/capi_ffi.h`).
use crate::guard;
use std::alloc::{Layout, alloc};
use std::ffi::c_char;

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_context_t {
    /// Empty, or the message bytes and a terminating NUL. A reader stops at the first NUL, as
    /// with the `std::string::c_str()` the C++ context returned.
    last_error: Vec<u8>,
}

#[unsafe(no_mangle)]
pub extern "C" fn adm_create_context() -> *mut adm_context_t {
    guard::pointer(|| unsafe {
        let context = alloc(Layout::new::<adm_context_t>()).cast::<adm_context_t>();
        if !context.is_null() {
            context.write(adm_context_t {
                last_error: Vec::new(),
            });
        }
        context
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_context(context: *mut adm_context_t) {
    if !context.is_null() {
        drop(unsafe { Box::from_raw(context) });
    }
}

/// The message lives until the next call that changes it, or the context's destruction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_context_last_error_message(
    context: *const adm_context_t,
) -> *const c_char {
    if context.is_null() {
        return c"".as_ptr();
    }
    let message = unsafe { &(*context).last_error };
    if message.is_empty() {
        c"".as_ptr()
    } else {
        message.as_ptr().cast()
    }
}

/// Replaces the last-error message with `len` bytes of `message` (0 clears it). NULL context: no-op.
/// Allocation failure returns INTERNAL and preserves the previous message.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_capi_context_set_error(
    context: *mut adm_context_t,
    message: *const u8,
    len: usize,
) -> guard::ErrorCode {
    guard::code(|| {
        let Some(context) = (unsafe { context.as_mut() }) else {
            return guard::OK;
        };
        if len == 0 || message.is_null() {
            context.last_error.clear();
            return guard::OK;
        }
        let Some(required) = len.checked_add(1) else {
            return guard::INTERNAL;
        };
        // Reserve the terminator too, before changing the message or borrowing the input.
        if context
            .last_error
            .try_reserve(required.saturating_sub(context.last_error.len()))
            .is_err()
        {
            return guard::INTERNAL;
        }
        let bytes = unsafe { std::slice::from_raw_parts(message, len) };
        context.last_error.clear();
        context.last_error.extend_from_slice(bytes);
        context.last_error.push(0);
        guard::OK
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::{GlobalAlloc, System};
    use std::cell::Cell;
    use std::ffi::CStr;

    thread_local! {
        static FAIL_NEXT_ALLOCATION: Cell<bool> = const { Cell::new(false) };
    }

    struct FaultAllocator;

    fn fail_allocation() -> bool {
        FAIL_NEXT_ALLOCATION
            .try_with(|fail| fail.replace(false))
            .unwrap_or(false)
    }

    unsafe impl GlobalAlloc for FaultAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            if fail_allocation() {
                std::ptr::null_mut()
            } else {
                unsafe { System.alloc(layout) }
            }
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
            if fail_allocation() {
                std::ptr::null_mut()
            } else {
                unsafe { System.realloc(ptr, layout, size) }
            }
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe { System.dealloc(ptr, layout) };
        }
    }

    #[global_allocator]
    static ALLOCATOR: FaultAllocator = FaultAllocator;

    fn with_allocation_failure<T>(body: impl FnOnce() -> T) -> (T, bool) {
        FAIL_NEXT_ALLOCATION.with(|fail| {
            assert!(!fail.replace(true));
            let result = body();
            (result, !fail.replace(false))
        })
    }

    fn message(context: *const adm_context_t) -> &'static [u8] {
        unsafe { CStr::from_ptr(adm_context_last_error_message(context)) }.to_bytes()
    }

    #[test]
    fn the_last_error_follows_the_setter() {
        let context = adm_create_context();
        assert!(!context.is_null());
        assert_eq!(message(context), b"");
        let text = "无效的房间配置: room";
        unsafe { mradm_capi_context_set_error(context, text.as_ptr(), text.len()) };
        assert_eq!(message(context), text.as_bytes());
        // A reader stops at an embedded NUL, as with c_str().
        unsafe { mradm_capi_context_set_error(context, b"a\0b".as_ptr(), 3) };
        assert_eq!(message(context), b"a");
        unsafe { mradm_capi_context_set_error(context, std::ptr::null(), 0) };
        assert_eq!(message(context), b"");
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn null_contexts_are_accepted() {
        assert_eq!(message(std::ptr::null()), b"");
        unsafe {
            mradm_capi_context_set_error(std::ptr::null_mut(), b"x".as_ptr(), 1);
            adm_destroy_context(std::ptr::null_mut());
        }
    }

    #[test]
    fn context_allocation_failure_returns_null_and_can_be_retried() {
        let (context, failed) = with_allocation_failure(|| adm_create_context());
        assert!(failed);
        assert!(context.is_null());
        let context = adm_create_context();
        assert!(!context.is_null());
        assert_eq!(message(context), b"");
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn message_allocation_failure_preserves_the_message_and_can_be_retried() {
        let context = adm_create_context();
        unsafe { mradm_capi_context_set_error(context, b"old".as_ptr(), 3) };
        let previous = unsafe { adm_context_last_error_message(context) };
        let text = [b'x'; 1024];
        let (code, failed) = with_allocation_failure(|| unsafe {
            mradm_capi_context_set_error(context, text.as_ptr(), text.len())
        });
        assert!(failed);
        assert_eq!(code, guard::INTERNAL);
        assert_eq!(unsafe { adm_context_last_error_message(context) }, previous);
        assert_eq!(message(context), b"old");
        assert_eq!(
            unsafe { mradm_capi_context_set_error(context, text.as_ptr(), text.len()) },
            guard::OK
        );
        assert_eq!(message(context), text);
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn clearing_a_message_does_not_allocate() {
        let context = adm_create_context();
        for text in ["", "previous error"] {
            unsafe { mradm_capi_context_set_error(context, text.as_ptr(), text.len()) };
            let (code, failed) = with_allocation_failure(|| unsafe {
                mradm_capi_context_set_error(context, std::ptr::null(), 0)
            });
            assert!(!failed);
            assert_eq!(code, guard::OK);
            assert_eq!(message(context), b"");
        }
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn impossible_message_lengths_preserve_the_message() {
        let context = adm_create_context();
        unsafe { mradm_capi_context_set_error(context, b"old".as_ptr(), 3) };
        for len in [usize::MAX, isize::MAX as usize] {
            assert_eq!(
                unsafe { mradm_capi_context_set_error(context, b"x".as_ptr(), len) },
                guard::INTERNAL
            );
            assert_eq!(message(context), b"old");
        }
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn output_string_allocation_failure_returns_null() {
        let (text, failed) = with_allocation_failure(|| unsafe {
            crate::string::mradm_capi_string(b"text".as_ptr(), 4)
        });
        assert!(failed);
        assert!(text.is_null());
    }
}
