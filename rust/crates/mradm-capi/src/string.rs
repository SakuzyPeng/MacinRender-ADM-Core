//! Strings the C ABI hands to the caller through `char**` outputs, released with `adm_free_string`
//! (`c_api.h`: never `free()` / `delete`). Every such string comes from here, so allocation and
//! release use the same allocator. The allocation keeps its size in a header
//! before the text.
use std::alloc::{Layout, alloc, dealloc};
use std::ffi::c_char;

const ALIGN: usize = align_of::<usize>();
const HEADER: usize = if size_of::<usize>() > ALIGN {
    size_of::<usize>()
} else {
    ALIGN
};

/// A copy of `len` bytes of `text` and a terminating NUL; NULL when out of memory.
pub(crate) unsafe fn copy(text: *const u8, len: usize) -> *mut c_char {
    let Some(size) = len.checked_add(HEADER + 1) else {
        return std::ptr::null_mut();
    };
    let Ok(layout) = Layout::from_size_align(size, ALIGN) else {
        return std::ptr::null_mut();
    };
    unsafe {
        let base = alloc(layout);
        if base.is_null() {
            return std::ptr::null_mut();
        }
        base.cast::<usize>().write(size);
        let out = base.add(HEADER);
        if len != 0 {
            std::ptr::copy_nonoverlapping(text, out, len);
        }
        out.add(len).write(0);
        out.cast()
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_free_string(s: *mut c_char) {
    if s.is_null() {
        return;
    }
    unsafe {
        let base = s.cast::<u8>().sub(HEADER);
        let size = base.cast::<usize>().read();
        dealloc(base, Layout::from_size_align_unchecked(size, ALIGN));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn strings_round_trip_through_free() {
        for text in ["", "{}", "无效的房间配置", &"x".repeat(100_000)] {
            let s = unsafe { copy(text.as_ptr(), text.len()) };
            assert!(!s.is_null());
            assert_eq!(unsafe { CStr::from_ptr(s) }.to_bytes(), text.as_bytes());
            assert_eq!(s.align_offset(ALIGN), 0);
            unsafe { adm_free_string(s) };
        }
        // Embedded NULs are copied; the length comes from the caller.
        let s = unsafe { copy(b"a\0b".as_ptr(), 3) };
        assert_eq!(
            unsafe { std::slice::from_raw_parts(s.cast::<u8>(), 4) },
            b"a\0b\0"
        );
        unsafe { adm_free_string(s) };
        unsafe { adm_free_string(std::ptr::null_mut()) };
    }

    #[test]
    fn impossible_lengths_are_null() {
        assert!(unsafe { copy(b"x".as_ptr(), usize::MAX) }.is_null());
        assert!(unsafe { copy(b"x".as_ptr(), isize::MAX as usize) }.is_null());
    }
}
