//! Exercise the actual Rayon parking state before any worker can incidentally
//! initialize it. Room preparation must not rely on a timed scheduling warmup.
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

mod sync {
    pub use std::sync::{Condvar, Mutex};
}
#[path = "../../../vendor/rayon-core/src/sleep/worker_sleep_state.rs"]
mod worker_sleep_state;

struct CheckedAllocator;
// All parking operations below execute on the calling test thread. A process-wide counter
// also observes libtest/background threads; the separate room_no_alloc test covers all DSP workers.
thread_local! { static ALLOCATIONS: Cell<Option<usize>> = const { Cell::new(None) }; }

fn note_allocation() {
    let _ = ALLOCATIONS.try_with(|count| {
        if let Some(n) = count.get() {
            count.set(Some(n + 1));
        }
    });
}

unsafe impl GlobalAlloc for CheckedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        note_allocation();
        unsafe { System.realloc(pointer, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: CheckedAllocator = CheckedAllocator;

fn count_allocations(call: impl FnOnce()) -> usize {
    struct Reset;
    impl Drop for Reset {
        fn drop(&mut self) {
            ALLOCATIONS.with(|count| count.set(None));
        }
    }
    ALLOCATIONS.with(|count| {
        assert!(count.get().is_none(), "allocation scopes cannot overlap");
        count.set(Some(0));
    });
    let _reset = Reset;
    call();
    ALLOCATIONS.with(|count| count.get().unwrap())
}

#[test]
fn allocation_counter_tracks_parking_thread_and_excludes_background_work() {
    let start = AtomicBool::new(false);
    let finished = AtomicBool::new(false);
    std::thread::scope(|threads| {
        threads.spawn(|| {
            while !start.load(Ordering::Acquire) {
                std::thread::yield_now();
            }
            for _ in 0..5 {
                std::hint::black_box(Box::new([0u8; 64]));
            }
            finished.store(true, Ordering::Release);
        });
        let background = count_allocations(|| {
            start.store(true, Ordering::Release);
            while !finished.load(Ordering::Acquire) {
                std::thread::yield_now();
            }
        });
        assert_eq!(
            background, 0,
            "unrelated threads must not enter the parking count"
        );
    });
    assert_eq!(
        count_allocations(|| {
            std::hint::black_box(Box::new([0u8; 64]));
        }),
        1,
        "an allocation on the parking thread must still be detected"
    );
    assert_eq!(
        count_allocations(|| {
            let mut samples = Vec::<u8>::with_capacity(1);
            samples.push(0);
            std::hint::black_box(&mut samples);
            samples.reserve_exact(32);
            std::hint::black_box(samples);
        }),
        2,
        "reallocation on the parking thread must still be detected"
    );
}

#[test]
fn first_park_and_wakeup_use_prepared_storage() {
    for workers in [1, 2, 3, 8, 32] {
        let states: Vec<worker_sleep_state::WorkerSleepState> =
            (0..workers).map(|_| Default::default()).collect();
        let allocations = count_allocations(|| {
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
        });
        assert_eq!(
            allocations, 0,
            "first wait/wakeup must not allocate after preparing {workers} workers"
        );
    }
}
