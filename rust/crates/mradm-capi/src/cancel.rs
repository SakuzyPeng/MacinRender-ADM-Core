//! `adm_cancel_token_t`: the C++ `std::stop_source` semantics. A render (or a preview session)
//! takes the token's current flag when it starts; `adm_cancel` sets the current flag from any
//! thread; `adm_reset_cancel_token` replaces it with a fresh one, leaving the flags already taken
//! as they are.
use crate::error::try_box;
use crate::guard;
use std::ptr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

/// One stop state: a request flag shared by the token and the renders that took it.
struct State {
    owners: AtomicUsize,
    stopped: AtomicBool,
}

/// An owner of a stop state.
pub(crate) struct Flag(*mut State);

// SAFETY: The state is atomics only, released by its last owner.
unsafe impl Send for Flag {}
unsafe impl Sync for Flag {}

impl Flag {
    /// A fresh state; `None` when out of memory.
    fn new() -> Option<Self> {
        let state = try_box(State {
            owners: AtomicUsize::new(1),
            stopped: AtomicBool::new(false),
        });
        (!state.is_null()).then(|| Self(state))
    }

    fn state(&self) -> &State {
        unsafe { &*self.0 }
    }

    pub(crate) fn stop_requested(&self) -> bool {
        self.state().stopped.load(Ordering::Acquire)
    }

    fn request_stop(&self) {
        self.state().stopped.store(true, Ordering::Release);
    }
}

impl Clone for Flag {
    fn clone(&self) -> Self {
        self.state().owners.fetch_add(1, Ordering::Relaxed);
        Self(self.0)
    }
}

impl Drop for Flag {
    fn drop(&mut self) {
        if self.state().owners.fetch_sub(1, Ordering::AcqRel) == 1 {
            drop(unsafe { Box::from_raw(self.0) });
        }
    }
}

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_cancel_token_t {
    current: Mutex<Flag>,
}

impl adm_cancel_token_t {
    /// The current flag, for a render about to start.
    pub(crate) fn take(&self) -> Flag {
        self.current
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn adm_create_cancel_token() -> *mut adm_cancel_token_t {
    guard::pointer(|| {
        Flag::new().map_or(ptr::null_mut(), |flag| {
            try_box(adm_cancel_token_t {
                current: Mutex::new(flag),
            })
        })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_cancel_token(token: *mut adm_cancel_token_t) {
    if !token.is_null() {
        drop(unsafe { Box::from_raw(token) });
    }
}

/// Thread-safe and idempotent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_cancel(token: *mut adm_cancel_token_t) {
    if let Some(token) = unsafe { token.as_ref() } {
        token
            .current
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .request_stop();
    }
}

/// A stop state cannot be un-requested: a fresh one replaces it for the renders that start next.
/// As the C++ (whose fresh `std::stop_source` threw out of a `noexcept` function), running out of
/// memory here ends the process.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_reset_cancel_token(token: *mut adm_cancel_token_t) {
    if let Some(token) = unsafe { token.as_ref() } {
        let Some(fresh) = Flag::new() else {
            std::process::abort();
        };
        *token.current.lock().unwrap_or_else(|e| e.into_inner()) = fresh;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn taken_flags_keep_their_state_across_a_reset() {
        let token = adm_create_cancel_token();
        let t = unsafe { &*token };
        let before = t.take();
        assert!(!before.stop_requested());
        unsafe { adm_cancel(token) };
        assert!(before.stop_requested());
        unsafe { adm_reset_cancel_token(token) };
        let after = t.take();
        assert!(before.stop_requested());
        assert!(!after.stop_requested());
        unsafe { adm_cancel(token) };
        assert!(after.stop_requested());
        unsafe { adm_destroy_cancel_token(token) };
        // The taken flags outlive the token.
        assert!(before.stop_requested() && after.stop_requested());
        unsafe {
            adm_cancel(ptr::null_mut());
            adm_reset_cancel_token(ptr::null_mut());
            adm_destroy_cancel_token(ptr::null_mut());
        }
    }

    #[test]
    fn creation_reports_allocation_failure() {
        let (token, failed) =
            crate::test_alloc::with_allocation_failure(|| adm_create_cancel_token());
        assert!(failed);
        assert!(token.is_null());
        let (token, failed) =
            crate::test_alloc::with_allocation_failure_after(1, || adm_create_cancel_token());
        assert!(failed);
        assert!(token.is_null());
    }
}
