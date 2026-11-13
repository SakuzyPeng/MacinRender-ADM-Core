//! Thread-local allocation failures for C ABI recovery tests.
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    static FAIL_AFTER: Cell<Option<usize>> = const { Cell::new(None) };
}

struct FaultAllocator;

fn fail_allocation() -> bool {
    FAIL_AFTER
        .try_with(|fail| match fail.get() {
            Some(0) => {
                fail.set(None);
                true
            }
            Some(remaining) => {
                fail.set(Some(remaining - 1));
                false
            }
            None => false,
        })
        .unwrap_or(false)
}

unsafe impl GlobalAlloc for FaultAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if fail_allocation() {
            std::ptr::null_mut()
        } else {
            unsafe { System.alloc(layout) }
        }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if fail_allocation() {
            std::ptr::null_mut()
        } else {
            unsafe { System.realloc(ptr, layout, size) }
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) };
    }
}

#[global_allocator]
static ALLOCATOR: FaultAllocator = FaultAllocator;

pub(crate) fn with_allocation_failure<T>(body: impl FnOnce() -> T) -> (T, bool) {
    with_allocation_failure_after(0, body)
}

pub(crate) fn with_allocation_failure_after<T>(
    successful: usize,
    body: impl FnOnce() -> T,
) -> (T, bool) {
    FAIL_AFTER.with(|fail| {
        assert!(fail.replace(Some(successful)).is_none());
        let result = body();
        (result, fail.replace(None).is_none())
    })
}
