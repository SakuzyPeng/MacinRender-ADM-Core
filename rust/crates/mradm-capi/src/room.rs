//! Room packages: `adm_room_load` / `adm_room_destroy` / `adm_room_manifest_json` and
//! `adm_room_inspect(_with_quality)`, through the room entries the C++ `RoomProfile` used.
//!
//! The exports still in `src/adm_c_api/adm_c_api.cpp` that take an `adm_room_config_t` read the
//! package through `mradm_capi_room_profile` (`src/adm_c_api/capi_ffi.h`).
use crate::context::adm_context_t;
use crate::error::{Error, Failure, c_str, caught, clear, report, try_box, until_nul};
use crate::guard;
use crate::string;
use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
use mradm_ffi::room::{
    RoomPreparedInfo, RoomProfileHandle, mradm_room_prepare_quality, mradm_room_prepared_destroy,
    mradm_room_prepared_info, mradm_room_prepared_json, mradm_room_profile_destroy,
    mradm_room_profile_json, mradm_room_profile_load, mradm_room_validate_screens,
};
use std::ffi::{CStr, c_char, c_void};
use std::ptr;

/// A loaded room package.
#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_room_t {
    profile: *mut RoomProfileHandle,
}

impl Drop for adm_room_t {
    fn drop(&mut self) {
        unsafe { mradm_room_profile_destroy(self.profile) };
    }
}

/// `adm_room_config_t` (`c_api.h`).
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_room_config_t {
    struct_size: u32,
    reserved: u32,
    room: *const adm_room_t,
    hrir_manifest: *const c_char,
    metres_per_unit: f64,
}

/// `adm_room_quality_t`.
const EXACT: i32 = 0;
const REALTIME: i32 = 1;

/// The C++ room checks' locator.
const ROOM: &[u8] = b"room-binaural";

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_room_load(
    context: *mut adm_context_t,
    manifest: *const c_char,
    out: *mut *mut adm_room_t,
) -> i32 {
    if !out.is_null() {
        unsafe { *out = ptr::null_mut() };
    }
    if context.is_null() || manifest.is_null() || out.is_null() {
        return guard::INVALID_ARGUMENT;
    }
    caught(context, None, || unsafe {
        clear(context);
        let path = c_str(manifest);
        let mut message = [0u8; 512];
        let mut raw = ptr::null_mut();
        let code = mradm_room_profile_load(
            path.as_ptr(),
            path.len(),
            &mut raw,
            message.as_mut_ptr(),
            message.len(),
        );
        if code != 0 {
            return report(
                context,
                Failure::error(code, until_nul(&message), ROOM),
                None,
            );
        }
        publish_room(raw, out)
    })
}

/// Takes ownership of the loaded profile, including when the ABI handle cannot be allocated.
unsafe fn publish_room(profile: *mut RoomProfileHandle, out: *mut *mut adm_room_t) -> i32 {
    let room = try_box(adm_room_t { profile });
    if room.is_null() {
        return guard::INTERNAL;
    }
    unsafe { *out = room };
    guard::OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_room_destroy(room: *mut adm_room_t) {
    if !room.is_null() {
        drop(unsafe { Box::from_raw(room) });
    }
}

/// Borrowed until the room's destruction; NULL for a NULL room.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_room_manifest_json(room: *const adm_room_t) -> *const c_char {
    match unsafe { room.as_ref() } {
        Some(room) => unsafe { mradm_room_profile_json(room.profile) }.cast(),
        None => ptr::null(),
    }
}

/// A validated `adm_room_config_t` (`to_room_config`).
pub(crate) struct Config<'a> {
    pub(crate) profile: *const RoomProfileHandle,
    pub(crate) hrir: &'a [u8],
    pub(crate) metres: f64,
}

pub(crate) unsafe fn config<'a>(config: *const adm_room_config_t) -> Result<Config<'a>, Failure> {
    let invalid = || Failure::error(1, "无效的房间配置".as_bytes(), b"");
    let Some(header) = (unsafe { config.cast::<u32>().as_ref() }) else {
        return Err(invalid());
    };
    if (*header as usize) < size_of::<adm_room_config_t>() {
        return Err(invalid());
    }
    let c = unsafe { &*config };
    let metres = c.metres_per_unit;
    let Some(room) = (unsafe { c.room.as_ref() }) else {
        return Err(invalid());
    };
    if c.reserved != 0 || !metres.is_finite() || (metres != 0.0 && metres < 0.01) || metres > 100.0
    {
        return Err(invalid());
    }
    let hrir = if c.hrir_manifest.is_null() {
        &[][..]
    } else {
        unsafe { c_str(c.hrir_manifest) }
    };
    Ok(Config {
        profile: room.profile,
        hrir,
        metres,
    })
}

/// The code a setter returns for a rejected config (`map_error`, no message).
pub(crate) fn failure_code(failure: Failure) -> i32 {
    match failure {
        Failure::Error(error) => crate::error::map(error.code),
        Failure::Exception => guard::INTERNAL,
    }
}

/// `validate_screen_config`: the reference (default when absent) and the reproduction (the
/// reference when absent) through the room's screen mapping; the code (0 when valid).
pub(crate) fn validate_screens(
    reference: Option<RoomScreenGeometry>,
    reproduction: Option<RoomScreenGeometry>,
) -> i32 {
    let reference = reference.unwrap_or_default();
    let screens = RoomScreens {
        enabled: 1,
        reserved: 0,
        reference,
        reproduction: reproduction.unwrap_or(reference),
    };
    let mut message = [0u8; 512];
    unsafe { mradm_room_validate_screens(&screens, 1, message.as_mut_ptr(), message.len()) }
}

/// `inspect_room`: the prepared room's JSON (no SOFA, no reuse).
unsafe fn inspect(config: &Config<'_>, quality: i32) -> Result<Vec<u8>, Failure> {
    let mut message = [0u8; 512];
    let mut raw = ptr::null_mut();
    let code = unsafe {
        mradm_room_prepare_quality(
            config.profile,
            config.hrir.as_ptr(),
            config.hrir.len(),
            b"".as_ptr(),
            0,
            ptr::null(),
            quality as u32,
            config.metres,
            &mut raw,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(Error::new(code, until_nul(&message), ROOM)?.into());
    }
    struct Prepared(*mut mradm_ffi::room::RoomPreparedHandle);
    impl Drop for Prepared {
        fn drop(&mut self) {
            unsafe { mradm_room_prepared_destroy(self.0) };
        }
    }
    let prepared = Prepared(raw);
    let mut info = RoomPreparedInfo::default();
    let code = unsafe { mradm_room_prepared_info(prepared.0, &mut info) };
    if code != 0 {
        return Err(Error::new(code, "房间 DSP 操作失败".as_bytes(), ROOM)?.into());
    }
    let json = unsafe { CStr::from_ptr(mradm_room_prepared_json(prepared.0).cast()) }.to_bytes();
    let mut copy = Vec::new();
    copy.try_reserve_exact(json.len())
        .map_err(|_| Failure::Exception)?;
    copy.extend_from_slice(json);
    Ok(copy)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_room_inspect(
    context: *mut adm_context_t,
    config: *const adm_room_config_t,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe { adm_room_inspect_with_quality(context, config, EXACT, out_json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_room_inspect_with_quality(
    context: *mut adm_context_t,
    config: *const adm_room_config_t,
    quality: i32,
    out_json: *mut *mut c_char,
) -> i32 {
    if !out_json.is_null() {
        unsafe { *out_json = ptr::null_mut() };
    }
    if context.is_null() || out_json.is_null() || !(quality == EXACT || quality == REALTIME) {
        return guard::INVALID_ARGUMENT;
    }
    caught(context, None, || unsafe {
        clear(context);
        let converted = match self::config(config) {
            Ok(converted) => converted,
            Err(failure) => return report(context, failure, None),
        };
        let json = match inspect(&converted, quality) {
            Ok(json) => json,
            Err(failure) => return report(context, failure, None),
        };
        let copy = string::copy(json.as_ptr(), json.len());
        if copy.is_null() {
            return guard::INTERNAL;
        }
        *out_json = copy;
        guard::OK
    })
}

/// A new owner of the room's package, for a C++ `RoomProfile` (destroyed with
/// `mradm_room_profile_destroy`); NULL for a NULL room or when out of memory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_capi_room_profile(room: *const adm_room_t) -> *mut c_void {
    guard::pointer(|| {
        unsafe { room.as_ref() }
            .and_then(|room| unsafe { &*room.profile }.share())
            .map_or(ptr::null_mut(), |handle| handle.cast())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::context::{adm_create_context, adm_destroy_context};
    use crate::test_alloc::with_allocation_failure;

    #[test]
    fn invalid_config_allocation_failure_returns_internal_and_allows_retry() {
        let context = adm_create_context();
        let mut out = ptr::null_mut();
        let (code, failed) =
            with_allocation_failure(|| unsafe { adm_room_inspect(context, ptr::null(), &mut out) });
        assert!(failed);
        assert_eq!(code, guard::INTERNAL);
        assert!(out.is_null());
        assert_eq!(
            unsafe { adm_room_inspect(context, ptr::null(), &mut out) },
            guard::INVALID_ARGUMENT
        );
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn handle_allocation_failure_releases_the_profile_once_and_allows_retry() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../tests/fixtures/room/synthetic/room.json");
        let path = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
        let mut profile = ptr::null_mut();
        let mut message = [0; 512];
        assert_eq!(
            unsafe {
                mradm_room_profile_load(
                    path.as_ptr().cast(),
                    path.as_bytes().len(),
                    &mut profile,
                    message.as_mut_ptr(),
                    message.len(),
                )
            },
            guard::OK
        );
        assert!(!profile.is_null());
        let mut out = ptr::null_mut();
        let (code, failed) = with_allocation_failure(|| unsafe { publish_room(profile, &mut out) });
        assert!(failed);
        assert_eq!(code, guard::INTERNAL);
        assert!(out.is_null());
        let context = adm_create_context();
        assert_eq!(
            unsafe { adm_room_load(context, path.as_ptr(), &mut out) },
            guard::OK
        );
        assert!(!unsafe { adm_room_manifest_json(out) }.is_null());
        unsafe {
            adm_room_destroy(out);
            adm_destroy_context(context);
        }
    }

    #[test]
    fn invalid_configs_are_rejected_before_any_read() {
        let room = adm_room_t {
            profile: ptr::null_mut(),
        };
        let valid = adm_room_config_t {
            struct_size: size_of::<adm_room_config_t>() as u32,
            reserved: 0,
            room: &room,
            hrir_manifest: ptr::null(),
            metres_per_unit: 0.0,
        };
        let check = |c: &adm_room_config_t| {
            unsafe { config(c) }.map(|c| c.metres).map_err(|e| match e {
                Failure::Error(e) => e.code,
                Failure::Exception => guard::INTERNAL,
            })
        };
        assert_eq!(check(&valid), Ok(0.0));
        for metres in [0.005, 100.5, f64::NAN, f64::INFINITY, -1.0] {
            let c = adm_room_config_t {
                metres_per_unit: metres,
                ..valid
            };
            assert_eq!(check(&c), Err(1), "{metres}");
        }
        for (metres, ok) in [(0.01, true), (100.0, true)] {
            let c = adm_room_config_t {
                metres_per_unit: metres,
                ..valid
            };
            assert_eq!(check(&c).is_ok(), ok);
        }
        assert_eq!(
            check(&adm_room_config_t {
                struct_size: 8,
                ..valid
            }),
            Err(1)
        );
        assert_eq!(
            check(&adm_room_config_t {
                reserved: 1,
                ..valid
            }),
            Err(1)
        );
        assert_eq!(
            check(&adm_room_config_t {
                room: ptr::null(),
                ..valid
            }),
            Err(1)
        );
        assert!(unsafe { config(ptr::null()) }.is_err());
        std::mem::forget(room);
    }

    #[test]
    fn null_rooms_have_no_manifest_or_profile() {
        assert!(unsafe { adm_room_manifest_json(ptr::null()) }.is_null());
        assert!(unsafe { mradm_capi_room_profile(ptr::null()) }.is_null());
        unsafe { adm_room_destroy(ptr::null_mut()) };
    }
}
