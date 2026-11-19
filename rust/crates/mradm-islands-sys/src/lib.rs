//! The C/C++ islands and third-party C libraries the Rust library links (6.6). This crate has no
//! items: its build script builds `src/CMakeLists.txt` of the repository through the `cmake` crate
//! (or reads the link manifest of a tree that already built it, `MRADM_NATIVE_LINK_FILE`) and hands
//! the archives and platform libraries to every crate that depends on it. `mradm-ffi` declares the
//! island entries it calls; the islands call Rust only through the services table it hands them.
#![no_std]
#![forbid(unsafe_code)]
