//! Narrow paths as interpreted by the MSVC filesystem library.

use std::{ffi::c_int, ptr};

const CP_UTF8: u32 = 65001;
const WC_NO_BEST_FIT_CHARS: u32 = 0x400;

unsafe extern "system" {
    fn AreFileApisANSI() -> c_int;
    fn GetACP() -> u32;
    fn GetOEMCP() -> u32;
    fn WideCharToMultiByte(
        code_page: u32,
        flags: u32,
        wide: *const u16,
        wide_len: c_int,
        narrow: *mut u8,
        narrow_len: c_int,
        default_char: *const u8,
        used_default_char: *mut c_int,
    ) -> c_int;
}

unsafe extern "C" {
    fn ___lc_codepage_func() -> u32;
}

/// MSVC's filesystem uses a UTF-8 C locale in preference to the Win32 file API code page.
pub fn code_page() -> u32 {
    unsafe {
        if ___lc_codepage_func() == CP_UTF8 {
            CP_UTF8
        } else if AreFileApisANSI() == 0 {
            GetOEMCP()
        } else {
            GetACP()
        }
    }
}

/// Encodes a path without substituting characters unavailable in the selected code page.
pub fn native_bytes(path: &str) -> Option<Vec<u8>> {
    let code_page = code_page();
    let mut bytes = Vec::new();
    if path.is_empty() || code_page == CP_UTF8 {
        bytes.try_reserve_exact(path.len()).ok()?;
        bytes.extend_from_slice(path.as_bytes());
        return Some(bytes);
    }

    let mut wide = Vec::new();
    wide.try_reserve_exact(path.len()).ok()?;
    wide.extend(path.encode_utf16());
    let len = c_int::try_from(wide.len()).ok()?;
    let convert = |out: *mut u8, out_len: c_int, used: *mut c_int| unsafe {
        WideCharToMultiByte(
            code_page,
            WC_NO_BEST_FIT_CHARS,
            wide.as_ptr(),
            len,
            out,
            out_len,
            ptr::null(),
            used,
        )
    };
    let size = convert(ptr::null_mut(), 0, ptr::null_mut());
    if size <= 0 {
        return None;
    }
    bytes.try_reserve_exact(size as usize).ok()?;
    bytes.resize(size as usize, 0);
    let mut used_default = 0;
    let written = convert(bytes.as_mut_ptr(), size, &mut used_default);
    (written == size && used_default == 0).then_some(bytes)
}
