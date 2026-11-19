//! The Rust services of the C++ islands (6.6 A): Rust hands the islands one table
//! (`mradm_islands_bind`) before it first calls one. The island tables' accessors bind it, and so
//! does every export's panic guard, so that the C++ island tests, which call the island wrappers
//! directly, bind it through any guarded C ABI call.
use mradm_ffi::api::islands::services;
use std::sync::Once;

#[cfg(not(test))]
unsafe extern "C" {
    fn mradm_islands_bind(services: *const mradm_ffi::api::islands::RustServices);
}

/// Binds the table once; later calls return at once.
pub(crate) fn bind() {
    static BIND: Once = Once::new();
    BIND.call_once(|| {
        let table = services();
        // SAFETY: the table is an immutable static, valid for the process lifetime.
        #[cfg(not(test))]
        unsafe {
            mradm_islands_bind(table)
        };
        #[cfg(test)]
        let _ = table;
    });
}
