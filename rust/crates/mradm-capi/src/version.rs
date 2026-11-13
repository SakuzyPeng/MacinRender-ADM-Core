//! `adm_api_version_*`: the `ADM_API_VERSION_*` macros of `c_api.h` (read by `build.rs`).
include!(concat!(env!("OUT_DIR"), "/version.rs"));

#[unsafe(no_mangle)]
pub extern "C" fn adm_api_version_major() -> i32 {
    MAJOR
}

#[unsafe(no_mangle)]
pub extern "C" fn adm_api_version_minor() -> i32 {
    MINOR
}

#[unsafe(no_mangle)]
pub extern "C" fn adm_api_version_patch() -> i32 {
    PATCH
}
