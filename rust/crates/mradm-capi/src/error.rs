//! Failures as the C++ exports reported them: an `mradm::Error` (code, message, locator) stored on
//! the context as `message: locator`, or an exception that the export's catch-all turned into
//! `ADM_ERROR_INTERNAL`.
use crate::context::{adm_context_t, mradm_capi_context_set_error};
use crate::guard;
use std::alloc::{Layout, alloc};
use std::ffi::{CStr, c_char};

/// An `mradm::Error`. `code` is the value the C++ cast to `ErrorCode`, possibly out of range.
pub(crate) struct Error {
    pub(crate) code: i32,
    pub(crate) message: Vec<u8>,
    pub(crate) context: Vec<u8>,
}

impl Error {
    pub(crate) fn new(code: i32, message: &[u8], context: &[u8]) -> Self {
        Self {
            code,
            message: message.to_vec(),
            context: context.to_vec(),
        }
    }

    /// `scene_error` / `wav_error`: codes outside 1–6 are `internal_error`.
    pub(crate) fn clamped(code: i32, message: &[u8], context: &[u8]) -> Self {
        let code = if (1..=6).contains(&code) {
            code
        } else {
            guard::INTERNAL
        };
        Self::new(code, message, context)
    }
}

/// What an export body ends in when it does not succeed.
pub(crate) enum Failure {
    Error(Error),
    /// A C++ exception: the export's catch-all handles it.
    Exception,
}

impl From<Error> for Failure {
    fn from(error: Error) -> Self {
        Self::Error(error)
    }
}

/// `map_error`: `adm_error_code_t` of an `ErrorCode`; unknown values are INTERNAL.
pub(crate) fn map(code: i32) -> i32 {
    if (0..=6).contains(&code) {
        code
    } else {
        guard::INTERNAL
    }
}

/// The bytes of a message buffer up to its NUL, as `reinterpret_cast<const char*>` read it.
pub(crate) fn until_nul(buffer: &[u8]) -> &[u8] {
    let end = buffer.iter().position(|&b| b == 0).unwrap_or(buffer.len());
    &buffer[..end]
}

/// A caller's NUL-terminated string.
pub(crate) unsafe fn c_str<'a>(text: *const c_char) -> &'a [u8] {
    unsafe { CStr::from_ptr(text) }.to_bytes()
}

pub(crate) unsafe fn set(context: *mut adm_context_t, message: &[u8]) -> bool {
    unsafe { mradm_capi_context_set_error(context, message.as_ptr(), message.len()) == guard::OK }
}

pub(crate) unsafe fn clear(context: *mut adm_context_t) {
    unsafe { set(context, &[]) };
}

/// `store_last_error`: `message: locator` (just the message without a locator). False when the
/// message could not be stored, where the C++ threw `bad_alloc`.
pub(crate) unsafe fn store(context: *mut adm_context_t, error: &Error) -> bool {
    let mut text = Vec::new();
    let len = error.message.len() + 2 + error.context.len();
    if text.try_reserve_exact(len).is_err() {
        return false;
    }
    text.extend_from_slice(&error.message);
    if !error.context.is_empty() {
        text.extend_from_slice(b": ");
        text.extend_from_slice(&error.context);
    }
    unsafe { set(context, &text) }
}

/// The result of an export that stores its errors on `context`: the error's code, or, for an
/// exception (or an error that could not be stored), INTERNAL with the catch-all's message.
pub(crate) unsafe fn report(
    context: *mut adm_context_t,
    failure: Failure,
    unexpected: Option<&str>,
) -> i32 {
    if let Failure::Error(error) = &failure
        && unsafe { store(context, error) }
    {
        return map(error.code);
    }
    if let Some(message) = unexpected {
        unsafe { set(context, message.as_bytes()) };
    }
    guard::INTERNAL
}

/// Runs an export body; a panic is the C++ catch-all: INTERNAL and its message.
pub(crate) fn caught(
    context: *mut adm_context_t,
    unexpected: Option<&str>,
    body: impl FnOnce() -> i32,
) -> i32 {
    guard::guard(None, || Some(body()))
        .unwrap_or_else(|| unsafe { report(context, Failure::Exception, unexpected) })
}

/// A heap object the caller releases through its destroy export; NULL when out of memory.
pub(crate) fn try_box<T>(value: T) -> *mut T {
    let object = unsafe { alloc(Layout::new::<T>()) }.cast::<T>();
    if !object.is_null() {
        unsafe { object.write(value) };
    }
    object
}

/// The UTF-8 spelling of a caller's narrow path (`wav_path_utf8`): Windows converts from the
/// code page `std::filesystem::path` uses; elsewhere the bytes are unchanged. `None` where the
/// C++ conversion threw.
#[cfg(not(windows))]
pub(crate) fn utf8_path(native: &[u8]) -> Option<Vec<u8>> {
    let mut path = Vec::new();
    path.try_reserve_exact(native.len()).ok()?;
    path.extend_from_slice(native);
    Some(path)
}

#[cfg(windows)]
pub(crate) fn utf8_path(native: &[u8]) -> Option<Vec<u8>> {
    unsafe extern "system" {
        fn AreFileApisANSI() -> i32;
        fn MultiByteToWideChar(
            code_page: u32,
            flags: u32,
            text: *const u8,
            text_len: i32,
            wide: *mut u16,
            wide_len: i32,
        ) -> i32;
    }
    unsafe extern "C" {
        fn ___lc_codepage_func() -> u32;
    }
    const CP_ACP: u32 = 0;
    const CP_OEMCP: u32 = 1;
    const CP_UTF8: u32 = 65001;
    const MB_ERR_INVALID_CHARS: u32 = 8;
    if native.is_empty() {
        return Some(Vec::new());
    }
    // The MSVC STL's __std_fs_code_page: the C locale's UTF-8, else the file APIs' code page.
    let code_page = unsafe {
        if ___lc_codepage_func() == CP_UTF8 {
            CP_UTF8
        } else if AreFileApisANSI() == 0 {
            CP_OEMCP
        } else {
            CP_ACP
        }
    };
    let len = i32::try_from(native.len()).ok()?;
    let wide_len = unsafe {
        MultiByteToWideChar(
            code_page,
            MB_ERR_INVALID_CHARS,
            native.as_ptr(),
            len,
            std::ptr::null_mut(),
            0,
        )
    };
    let count = usize::try_from(wide_len).ok().filter(|&n| n != 0)?;
    let mut wide = Vec::new();
    wide.try_reserve_exact(count).ok()?;
    wide.resize(count, 0u16);
    let written = unsafe {
        MultiByteToWideChar(
            code_page,
            MB_ERR_INVALID_CHARS,
            native.as_ptr(),
            len,
            wide.as_mut_ptr(),
            wide_len,
        )
    };
    if written != wide_len {
        return None;
    }
    String::from_utf16(&wide).ok().map(String::into_bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_map_as_map_error_did() {
        assert_eq!((0..=6).map(map).collect::<Vec<_>>(), [0, 1, 2, 3, 4, 5, 6]);
        assert_eq!(map(7), guard::INTERNAL);
        assert_eq!(map(-1), guard::INTERNAL);
        assert_eq!(Error::clamped(0, b"", b"").code, guard::INTERNAL);
        assert_eq!(Error::clamped(3, b"", b"").code, 3);
    }

    #[test]
    fn messages_stop_at_the_nul() {
        assert_eq!(until_nul(b"abc\0def"), b"abc");
        assert_eq!(until_nul(b"abc"), b"abc");
    }

    #[test]
    fn stored_errors_join_message_and_locator() {
        let context = crate::context::adm_create_context();
        let message = |c| unsafe {
            CStr::from_ptr(crate::context::adm_context_last_error_message(c))
                .to_bytes()
                .to_vec()
        };
        unsafe {
            assert!(store(context, &Error::new(3, b"io", b"input=a.wav")));
            assert_eq!(message(context), b"io: input=a.wav");
            assert!(store(
                context,
                &Error::new(1, "无效的房间配置".as_bytes(), b"")
            ));
            assert_eq!(message(context), "无效的房间配置".as_bytes());
            assert_eq!(
                report(context, Failure::Exception, Some("unexpected")),
                guard::INTERNAL
            );
            assert_eq!(message(context), b"unexpected");
            assert_eq!(
                report(context, Error::new(9, b"x", b"").into(), None),
                guard::INTERNAL
            );
            crate::context::adm_destroy_context(context);
        }
    }

    #[cfg(not(windows))]
    #[test]
    fn paths_are_unchanged_off_windows() {
        assert_eq!(
            utf8_path("a/ü.wav".as_bytes()).unwrap(),
            "a/ü.wav".as_bytes()
        );
        assert_eq!(utf8_path(b"\xff").unwrap(), b"\xff");
    }
}
