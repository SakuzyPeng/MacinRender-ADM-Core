//! Panics never cross the C ABI. A panic inside an export ends that call the way an exception
//! does in the C++ exports (`catch (...)`): the export returns its fallback, normally
//! `ADM_ERROR_INTERNAL`, NULL or nothing. Callback threads and reentrancy are the caller's, as
//! before; the guard holds no state.
// The exports that use it move here family by family.
#![allow(dead_code)]

use std::panic::{AssertUnwindSafe, catch_unwind};

/// `adm_error_code_t` (stable values, `c_api.h`).
pub(crate) type ErrorCode = i32;
pub(crate) const OK: ErrorCode = 0;
pub(crate) const INVALID_ARGUMENT: ErrorCode = 1;
pub(crate) const UNSUPPORTED: ErrorCode = 2;
pub(crate) const IO: ErrorCode = 3;
pub(crate) const RENDER_FAILED: ErrorCode = 4;
pub(crate) const CANCELLED: ErrorCode = 5;
pub(crate) const INTERNAL: ErrorCode = 6;

/// Runs one export body; a panic returns `fallback`.
pub(crate) fn guard<T>(fallback: T, body: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(body)).unwrap_or(fallback)
}

/// An export returning an error code; a panic is `ADM_ERROR_INTERNAL`.
pub(crate) fn code(body: impl FnOnce() -> ErrorCode) -> ErrorCode {
    guard(INTERNAL, body)
}

/// An export returning a pointer; a panic is NULL.
pub(crate) fn pointer<T>(body: impl FnOnce() -> *mut T) -> *mut T {
    guard(std::ptr::null_mut(), body)
}

/// An export returning nothing; a panic is swallowed.
pub(crate) fn unit(body: impl FnOnce()) {
    guard((), body);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn panics_become_the_export_fallback() {
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        assert_eq!(code(|| panic!("export")), INTERNAL);
        assert!(pointer::<u8>(|| panic!("export")).is_null());
        let mut reached = false;
        unit(|| {
            reached = true;
            panic!("export");
        });
        assert!(reached);
        assert_eq!(guard(-1, || -> i32 { panic!("export") }), -1);
        std::panic::set_hook(hook);
    }

    #[test]
    fn results_pass_through() {
        assert_eq!(code(|| UNSUPPORTED), UNSUPPORTED);
        let mut value = 7_u8;
        assert_eq!(pointer(|| &raw mut value), &raw mut value);
        assert_eq!(guard(0, || 3), 3);
        assert_eq!(
            [
                OK,
                INVALID_ARGUMENT,
                UNSUPPORTED,
                IO,
                RENDER_FAILED,
                CANCELLED,
                INTERNAL
            ],
            [0, 1, 2, 3, 4, 5, 6]
        );
    }
}
