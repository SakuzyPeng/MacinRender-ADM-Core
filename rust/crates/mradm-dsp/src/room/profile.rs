//! Opt-in stage wall times, summed across workers. Nested stages overlap; totals
//! are not CPU time and must not be added together. An optional diagnostic clock
//! reports CPU time on the thread executing each stage. Reset/snapshot only while idle.
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
    VoiceRender,
    VoiceBatch,
}
pub const STAGE_COUNT: usize = 14;
pub const NAMES: [&str; STAGE_COUNT] = [
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
    "voice_render",
    "voice_batch",
];
#[cfg(feature = "room-profile")]
mod enabled {
    use super::{STAGE_COUNT, Stage};
    use std::{
        marker::PhantomData,
        rc::Rc,
        sync::{
            OnceLock,
            atomic::{AtomicU64, Ordering},
        },
        time::Instant,
    };
    static CPU_CLOCK: OnceLock<fn() -> Option<u64>> = OnceLock::new();
    // Each room worker updates its own counters. Shared stage atomics otherwise
    // distort the very scheduling/memory behavior that this profiler measures.
    // Slot zero also safely aggregates any callers outside the 32-worker pool.
    const THREAD_SLOTS: usize = 33;
    #[repr(align(128))]
    struct Counter {
        calls: AtomicU64,
        nanos: AtomicU64,
        cpu_calls: AtomicU64,
        cpu_nanos: AtomicU64,
    }
    static COUNTERS: [[Counter; STAGE_COUNT]; THREAD_SLOTS] = [const {
        [const {
            Counter {
                calls: AtomicU64::new(0),
                nanos: AtomicU64::new(0),
                cpu_calls: AtomicU64::new(0),
                cpu_nanos: AtomicU64::new(0),
            }
        }; STAGE_COUNT]
    }; THREAD_SLOTS];
    pub struct Guard {
        stage: Stage,
        start: Option<Instant>,
        cpu_start: Option<u64>,
        thread_slot: usize,
        // A thread CPU interval must end on the thread where it began.
        _thread_bound: PhantomData<Rc<()>>,
    }
    impl Guard {
        #[inline(always)]
        pub fn new(stage: Stage) -> Self {
            if cfg!(feature = "room-trace")
                && matches!(
                    stage,
                    Stage::HrirQuery
                        | Stage::SourceResponse
                        | Stage::TargetFilters
                        | Stage::EmitterBegin
                        | Stage::EmitterConvolution
                        | Stage::SourceReduction
                )
            {
                return Self {
                    stage,
                    start: None,
                    cpu_start: None,
                    thread_slot: 0,
                    _thread_bound: PhantomData,
                };
            }
            Self {
                stage,
                start: Some(Instant::now()),
                // CPU clocks cover whole voice jobs rather than each nested
                // response/filter/convolution operation. This bounds syscall
                // overhead while retaining a wall/CPU pair for every worker job.
                cpu_start: if matches!(
                    stage,
                    Stage::SessionProcess
                        | Stage::CanonicalBlock
                        | Stage::WorkerRoundtrip
                        | Stage::WorkerExecution
                        | Stage::VoiceRender
                        | Stage::VoiceBatch
                        | Stage::LateField
                        | Stage::LateDecoder
                ) {
                    CPU_CLOCK.get().and_then(|clock| clock())
                } else {
                    None
                },
                thread_slot: rayon::current_thread_index()
                    .filter(|i| *i < THREAD_SLOTS - 1)
                    .map_or(0, |i| i + 1),
                _thread_bound: PhantomData,
            }
        }
    }
    impl Drop for Guard {
        #[inline(always)]
        fn drop(&mut self) {
            let Some(start) = self.start else { return };
            let counter = &COUNTERS[self.thread_slot][self.stage as usize];
            let nanos = start.elapsed().as_nanos() as u64;
            if let Some(cpu) = self.cpu_start.and_then(|start| {
                CPU_CLOCK
                    .get()
                    .and_then(|clock| clock())
                    .and_then(|end| end.checked_sub(start))
            }) {
                counter.cpu_nanos.fetch_add(cpu, Ordering::Relaxed);
                counter.cpu_calls.fetch_add(1, Ordering::Relaxed);
            }
            counter.nanos.fetch_add(nanos, Ordering::Relaxed);
            counter.calls.fetch_add(1, Ordering::Relaxed);
        }
    }
    pub fn reset() {
        for counter in COUNTERS.iter().flatten() {
            counter.calls.store(0, Ordering::Relaxed);
            counter.nanos.store(0, Ordering::Relaxed);
            counter.cpu_calls.store(0, Ordering::Relaxed);
            counter.cpu_nanos.store(0, Ordering::Relaxed);
        }
    }
    pub fn snapshot() -> [(u64, u64); STAGE_COUNT] {
        std::array::from_fn(|i| {
            (
                COUNTERS
                    .iter()
                    .map(|thread| thread[i].calls.load(Ordering::Relaxed))
                    .sum(),
                COUNTERS
                    .iter()
                    .map(|thread| thread[i].nanos.load(Ordering::Relaxed))
                    .sum(),
            )
        })
    }
    /// Install during preparation only. The clock returns this thread's CPU
    /// nanoseconds without allocation; None means the observation is unavailable.
    /// Native clock bindings belong to the caller, outside the safe DSP crate.
    pub fn set_thread_cpu_clock(clock: fn() -> Option<u64>) -> bool {
        CPU_CLOCK.set(clock).is_ok()
    }
    /// CPU intervals overlap for nested stages, just like the wall intervals.
    pub fn cpu_snapshot() -> [(u64, u64); STAGE_COUNT] {
        std::array::from_fn(|i| {
            (
                COUNTERS
                    .iter()
                    .map(|thread| thread[i].cpu_calls.load(Ordering::Relaxed))
                    .sum(),
                COUNTERS
                    .iter()
                    .map(|thread| thread[i].cpu_nanos.load(Ordering::Relaxed))
                    .sum(),
            )
        })
    }
}
#[cfg(feature = "room-profile")]
pub use enabled::{Guard, cpu_snapshot, reset, set_thread_cpu_clock, snapshot};
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
pub fn snapshot() -> [(u64, u64); STAGE_COUNT] {
    [(0, 0); STAGE_COUNT]
}
#[cfg(not(feature = "room-profile"))]
pub fn cpu_snapshot() -> [(u64, u64); STAGE_COUNT] {
    [(0, 0); STAGE_COUNT]
}
#[cfg(not(feature = "room-profile"))]
pub fn set_thread_cpu_clock(_: fn() -> Option<u64>) -> bool {
    false
}

#[cfg(not(feature = "room-profile"))]
impl Drop for Guard {
    #[inline(always)]
    fn drop(&mut self) {}
}
