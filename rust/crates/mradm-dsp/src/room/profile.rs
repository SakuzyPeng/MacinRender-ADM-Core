//! Opt-in stage wall times, summed across workers. Nested stages overlap; totals
//! are not CPU time and must not be added together. Reset/snapshot only while idle.
//! Without `room-profile`, guards compile away and do not read a clock.
#[derive(Clone, Copy)]
#[repr(usize)]
pub enum Stage {
    SessionProcess,
    CanonicalBlock,
    HrirQuery,
    SourceResponse,
    TargetFilters,
    EmitterBegin,
    EmitterConvolution,
    WorkerRoundtrip,
    WorkerExecution,
    SourceReduction,
    LateField,
    LateDecoder,
}
pub const NAMES: [&str; 12] = [
    "session_process",
    "canonical_block",
    "hrir_query",
    "source_response",
    "target_filters",
    "emitter_begin",
    "emitter_convolution",
    "worker_roundtrip",
    "worker_execution",
    "source_reduction",
    "late_field",
    "late_decoder",
];
#[cfg(feature = "room-profile")]
mod enabled {
    use super::Stage;
    use std::{
        sync::atomic::{AtomicU64, Ordering},
        time::Instant,
    };
    // Separate cache lines avoid false sharing between different stages.
    #[repr(align(128))]
    struct Counter {
        calls: AtomicU64,
        nanos: AtomicU64,
    }
    static COUNTERS: [Counter; 12] = [const {
        Counter {
            calls: AtomicU64::new(0),
            nanos: AtomicU64::new(0),
        }
    }; 12];
    pub struct Guard {
        stage: Stage,
        start: Instant,
    }
    impl Guard {
        pub fn new(stage: Stage) -> Self {
            Self {
                stage,
                start: Instant::now(),
            }
        }
    }
    impl Drop for Guard {
        fn drop(&mut self) {
            let counter = &COUNTERS[self.stage as usize];
            counter
                .nanos
                .fetch_add(self.start.elapsed().as_nanos() as u64, Ordering::Relaxed);
            counter.calls.fetch_add(1, Ordering::Relaxed);
        }
    }
    pub fn reset() {
        for counter in &COUNTERS {
            counter.calls.store(0, Ordering::Relaxed);
            counter.nanos.store(0, Ordering::Relaxed);
        }
    }
    pub fn snapshot() -> [(u64, u64); 12] {
        std::array::from_fn(|i| {
            (
                COUNTERS[i].calls.load(Ordering::Relaxed),
                COUNTERS[i].nanos.load(Ordering::Relaxed),
            )
        })
    }
}
#[cfg(feature = "room-profile")]
pub use enabled::{Guard, reset, snapshot};
#[cfg(not(feature = "room-profile"))]
pub struct Guard;
#[cfg(not(feature = "room-profile"))]
impl Guard {
    #[inline(always)]
    pub fn new(_: Stage) -> Self {
        Self
    }
}
#[cfg(not(feature = "room-profile"))]
pub fn reset() {}
#[cfg(not(feature = "room-profile"))]
pub fn snapshot() -> [(u64, u64); 12] {
    [(0, 0); 12]
}

#[cfg(not(feature = "room-profile"))]
impl Drop for Guard {
    #[inline(always)]
    fn drop(&mut self) {}
}
