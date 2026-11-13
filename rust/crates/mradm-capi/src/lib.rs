//! The public C ABI (`include/adm/c_api.h`) on the Rust side. Each export implemented here is
//! removed from `src/adm_c_api/adm_c_api.cpp` in the same change; the shared library takes every
//! declared function from whichever side defines it, and `scripts/quality/check-ffi-headers.py`
//! compares the Rust signatures with `c_api.h`.
//!
//! This is also the staticlib CMake links: it carries mradm-ffi, whose private `mradm_*` entries
//! the C++ shells and islands still call.
#![allow(clippy::missing_safety_doc)]

mod cancel;
mod context;
mod error;
mod guard;
mod options;
mod query;
mod render;
mod room;
mod scene;
mod string;
mod version;

#[cfg(test)]
mod test_alloc;
