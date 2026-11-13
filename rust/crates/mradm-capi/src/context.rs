//! `adm_context_t`: the caller's handle for the render, inspect, preview and monitor entries, holding
//! the message of the last failed call (`adm_context_last_error_message`). A context is used by one
//! thread at a time, as before.
//!
//! The exports still in `src/adm_c_api/adm_c_api.cpp` set the message through
//! `mradm_capi_context_set_error` (`src/adm_c_api/capi_ffi.h`).
use crate::guard;
use std::ffi::c_char;

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_context_t {
    /// The message bytes and a terminating NUL; a reader stops at the first NUL, as with the
    /// `std::string::c_str()` the C++ context returned.
    last_error: Vec<u8>,
}

#[unsafe(no_mangle)]
pub extern "C" fn adm_create_context() -> *mut adm_context_t {
    guard::pointer(|| {
        Box::into_raw(Box::new(adm_context_t {
            last_error: vec![0],
        }))
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
    unsafe { (*context).last_error.as_ptr().cast() }
}

/// Replaces the last-error message with `len` bytes of `message` (0 clears it). NULL context: no-op.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_capi_context_set_error(
    context: *mut adm_context_t,
    message: *const u8,
    len: usize,
) {
    guard::unit(|| {
        let Some(context) = (unsafe { context.as_mut() }) else {
            return;
        };
        let bytes = if len == 0 || message.is_null() {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(message, len) }
        };
        context.last_error.clear();
        context.last_error.extend_from_slice(bytes);
        context.last_error.push(0);
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

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
}
