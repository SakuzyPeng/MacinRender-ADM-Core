//! The public C ABI (`include/adm/c_api.h`): every declared function is implemented here, and
//! `scripts/quality/check-ffi-headers.py` compares the Rust signatures with `c_api.h`. The C++ left in
//! `src/adm_c_api/adm_c_api.cpp` is the progress-callback trampolines, which keep a C++ caller's
//! exception on the C++ side.
//!
//! The exports call the Rust crates directly (R2); what needs a C table, a callback that keeps an
//! address or a platform service comes from `mradm_ffi::api`, never from its `mradm_*` entries.
//! Built as the shared `libmradm_capi` (a cdylib: rustc exports exactly the `adm_*` functions, as no
//! other crate of the graph has a `#[no_mangle]` function), as the staticlib the C++ tests link, and
//! as the library of the `mradm` executable (6.6).
#![allow(clippy::missing_safety_doc)]

mod cancel;
mod cli;
mod context;
mod error;
mod guard;
mod monitor;
mod options;
mod osc;
mod query;
mod render;
mod room;
mod scene;
mod services;
mod string;
mod version;

/// Runs the `mradm` command line of this process and returns its exit code.
pub fn cli_main() -> i32 {
    cli::main()
}

#[cfg(test)]
mod test_alloc;
#[cfg(test)]
mod tests;
