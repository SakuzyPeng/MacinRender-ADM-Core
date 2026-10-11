//! Room-only numerical contract. Resource I/O belongs to the caller; preparation
//! accepts authenticated bytes and processing owns all of its mutable history.
pub mod control;
pub mod geometry;
pub mod hrir;
pub mod partition;
pub mod prepared;
pub mod resources;
pub mod session;
pub mod signal;
pub mod spatial;
mod spectral;
mod workers;

pub const SAMPLE_RATE: u32 = 48_000;
pub const BLOCK: usize = 256;
pub const FILTER_FADE: usize = 480;
pub const ROOM_FADE: usize = 4_800;

// Preparation-only platform setup for the dedicated room worker threads.
pub fn set_worker_start_hook(hook: fn()) {
    workers::set_start_hook(hook);
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
