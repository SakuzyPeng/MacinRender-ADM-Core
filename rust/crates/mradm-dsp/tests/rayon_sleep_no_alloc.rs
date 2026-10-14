//! Exercise the actual Rayon parking state before any worker can incidentally
//! initialize it. Room preparation must not rely on a timed scheduling warmup.
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::Duration;

mod sync {
    pub use std::sync::{Condvar, Mutex};
}
#[path = "../../../vendor/rayon-core/src/sleep/worker_sleep_state.rs"]
mod worker_sleep_state;

struct CheckedAllocator;
static CHECK: AtomicBool = AtomicBool::new(false);
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);
unsafe impl GlobalAlloc for CheckedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if CHECK.load(Ordering::Relaxed) {
            ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if CHECK.load(Ordering::Relaxed) {
            ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.realloc(pointer, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: CheckedAllocator = CheckedAllocator;

#[test]
fn first_park_and_wakeup_use_prepared_storage() {
    for workers in [1, 2, 3, 8, 32] {
        let states: Vec<worker_sleep_state::WorkerSleepState> =
            (0..workers).map(|_| Default::default()).collect();
        ALLOCATIONS.store(0, Ordering::SeqCst);
        CHECK.store(true, Ordering::SeqCst);
        for state in &states {
            let blocked = state.is_blocked.lock().unwrap();
            let (blocked, _) = state
                .condvar
                .wait_timeout(blocked, Duration::from_millis(1))
                .unwrap();
            assert!(!*blocked);
            drop(blocked);
            state.condvar.notify_one();
            state.condvar.notify_all();
        }
        CHECK.store(false, Ordering::SeqCst);
        assert_eq!(
            ALLOCATIONS.load(Ordering::SeqCst),
            0,
            "first wait/wakeup must not allocate after preparing {workers} workers"
        );
    }
}
