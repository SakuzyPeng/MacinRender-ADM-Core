//! The public C ABI (`include/adm/c_api.h`): every declared function is implemented here, and
//! `scripts/quality/check-ffi-headers.py` compares the Rust signatures with `c_api.h`. The C++ left in
//! `src/adm_c_api/adm_c_api.cpp` is the progress-callback trampolines, which keep a C++ caller's
//! exception on the C++ side.
//!
//! This is also the staticlib CMake links: it carries mradm-ffi, whose private `mradm_*` entries
//! the C++ shells and islands still call.
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
mod string;
mod version;

#[cfg(test)]
mod test_alloc;
