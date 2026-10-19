//! Room-only numerical contract. Resource I/O belongs to the caller; preparation
//! accepts authenticated bytes and processing owns all of its mutable history.
pub mod collection;
pub mod control;
pub mod geometry;
#[cfg(feature = "room-gpu-experiment")]
pub mod gpu_experiment;
pub mod hrir;
pub mod partition;
pub mod prepared;
pub mod profile;
pub mod quality;
pub mod resources;
pub mod screen;
pub mod session;
pub mod signal;
pub mod sofa;
pub mod spatial;
mod spectral;
mod workers;

pub const SAMPLE_RATE: u32 = 48_000;
pub const BLOCK: usize = 256;
pub const FILTER_FADE: usize = 480;
pub const ROOM_FADE: usize = 4_800;

/// Supported processing clocks. Source SOFA rates may differ during preparation.
pub fn validate_sample_rate(rate: u32) -> crate::Result<()> {
    if matches!(rate, 48_000 | 96_000) {
        Ok(())
    } else {
        Err(crate::Error::Unsupported("房间处理仅支持 48 或 96 kHz"))
    }
}

pub fn filter_fade(rate: u32) -> usize {
    rate as usize / 100
}
pub fn room_fade(rate: u32) -> usize {
    rate as usize / 10
}
pub fn diffuse_tail(rate: u32) -> usize {
    (crate::diffuse::DELAY_LENGTH - 1) * (rate / SAMPLE_RATE) as usize
}

// Preparation-only platform setup for the dedicated room worker threads.
pub fn set_worker_start_hook(hook: fn()) {
    workers::set_start_hook(hook);
}
/// Verification only: run `job` once on the shared room workers through the
/// session's mailbox, so branches taken only inside the pool (the parallel
/// late pass) can be compared with their serial form. Allocates per call;
/// audio processing never uses it.
#[doc(hidden)]
pub fn run_on_workers<F>(job: F) -> crate::Result<()>
where
    F: FnMut() -> crate::Result<()> + Send + 'static,
{
    struct Job<F>(std::sync::Mutex<F>);
    impl<F: FnMut() -> crate::Result<()> + Send> workers::Work for Job<F> {
        fn run(&self) -> crate::Result<()> {
            (self.0.lock().unwrap())()
        }
    }
    workers::shared()?.run(std::sync::Arc::new(Job(std::sync::Mutex::new(job))))
}
/// Commit newly allocated scratch pages during preparation. Zero-filled virtual
/// allocations otherwise defer page faults until the first audible switch.
pub(crate) fn touch_pages<T: Copy>(values: &mut [T]) {
    let stride = (4096 / size_of::<T>().max(1)).max(1);
    for index in (0..values.len()).step_by(stride) {
        let value = values[index];
        *std::hint::black_box(&mut values[index]) = std::hint::black_box(value);
    }
    if let Some(value) = values.last_mut() {
        let copy = *value;
        *std::hint::black_box(value) = std::hint::black_box(copy);
    }
}
