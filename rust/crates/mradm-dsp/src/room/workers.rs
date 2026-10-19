//! A persistent Rayon driver with a fixed owned-work mailbox. No borrowed
//! pointer or external allocating injector is used during room processing.
use crate::{Error, Result};
use rayon::prelude::*;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Condvar, Mutex},
};
static START_HOOK: std::sync::OnceLock<fn()> = std::sync::OnceLock::new();
pub fn set_start_hook(hook: fn()) {
    let _ = START_HOOK.set(hook);
}
pub fn shared() -> Result<Arc<Workers>> {
    static POOL: std::sync::OnceLock<Option<Arc<Workers>>> = std::sync::OnceLock::new();
    POOL.get_or_init(|| Workers::new().ok().map(Arc::new))
        .clone()
        .ok_or(Error::RenderFailed("Cannot prepare room DSP workers"))
}
struct Noop;
impl Work for Noop {
    fn run(&self) -> Result<()> {
        Ok(())
    }
}
pub trait Work: Send + Sync {
    fn run(&self) -> Result<()>;
}
struct Mailbox {
    job: Option<Arc<dyn Work>>,
    result: Option<Result<()>>,
    stop: bool,
}
struct Shared {
    state: Mutex<Mailbox>,
    changed: Condvar,
}
#[cfg(feature = "room-gpu-experiment")]
struct Pending<'a> {
    shared: &'a Shared,
    finished: bool,
}
#[cfg(feature = "room-gpu-experiment")]
impl Pending<'_> {
    fn finish(&mut self) -> Result<()> {
        let mut state = self.shared.state.lock().unwrap();
        while state.result.is_none() {
            state = self.shared.changed.wait(state).unwrap();
        }
        self.finished = true;
        state.result.take().unwrap()
    }
}
#[cfg(feature = "room-gpu-experiment")]
impl Drop for Pending<'_> {
    fn drop(&mut self) {
        if !self.finished {
            // Drain even when preparation unwinds, before releasing the serial
            // mailbox guard. A later session must not consume this job's result.
            let _ = self.finish();
        }
    }
}
pub struct Workers {
    shared: Arc<Shared>,
    serial: Mutex<()>,
    _pool: rayon::ThreadPool,
    noop: Arc<Noop>,
}
impl Workers {
    pub fn new() -> Result<Self> {
        // Rust's platform query accounts for Linux CPU affinity and cgroup quota.
        // Keep the established desktop policy; Linux can use its effective quota.
        let maximum = if cfg!(target_os = "linux") { 32 } else { 8 };
        let default_count = std::thread::available_parallelism()
            .map_or(1, usize::from)
            .min(maximum);
        // Preparation-only diagnostic override. Worker count never changes the
        // arithmetic or the fixed reduction order, and cannot change mid-session.
        let count = match std::env::var("MR_ADM_ROOM_WORKERS") {
            Ok(value) => value
                .parse::<usize>()
                .ok()
                .filter(|n| (1..=32).contains(n))
                .ok_or(Error::InvalidArgument("MR_ADM_ROOM_WORKERS must be 1..32"))?,
            Err(std::env::VarError::NotPresent) => default_count,
            Err(_) => return Err(Error::InvalidArgument("Invalid room worker count")),
        };
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(count)
            .thread_name(|i| format!("mradm-room-{i}"))
            .start_handler(|_| {
                if let Some(hook) = START_HOOK.get() {
                    hook();
                }
            })
            .build()
            .map_err(|_| Error::RenderFailed("Cannot prepare room workers"))?;
        // Start every thread while allocations are permitted. The vendored
        // rayon-core prepares Apple's worker parking locks in its constructor;
        // no timed join is needed to make each worker happen to go to sleep.
        pool.broadcast(|_| {
            (0..512usize).into_par_iter().for_each(|i| {
                std::hint::black_box(i);
            });
        });
        let shared = Arc::new(Shared {
            state: Mutex::new(Mailbox {
                job: None,
                result: None,
                stop: false,
            }),
            changed: Condvar::new(),
        });
        // Prepare our mailbox's parking pair as well. A fast Noop may finish
        // before the submitting thread ever waits for it.
        #[cfg(target_vendor = "apple")]
        drop(
            shared
                .changed
                .wait_timeout(shared.state.lock().unwrap(), std::time::Duration::ZERO)
                .unwrap(),
        );
        let driver = shared.clone();
        pool.spawn(move || {
            loop {
                let mut state = driver.state.lock().unwrap();
                while state.job.is_none() && !state.stop {
                    state = driver.changed.wait(state).unwrap();
                }
                if state.stop {
                    break;
                }
                let job = state.job.take().unwrap();
                drop(state);
                let profile = super::profile::Guard::new(super::profile::Stage::WorkerExecution);
                let result = catch_unwind(AssertUnwindSafe(|| job.run()))
                    .unwrap_or(Err(Error::RenderFailed("Room worker panicked")));
                drop(profile);
                // Release the job before signalling completion. The session keeps
                // the owning Arc; ordinary dispatch never destroys DSP resources.
                drop(job);
                let mut state = driver.state.lock().unwrap();
                state.result = Some(result);
                driver.changed.notify_all();
            }
        });
        let result = Self {
            shared,
            serial: Mutex::new(()),
            _pool: pool,
            noop: Arc::new(Noop),
        };
        result.warm()?;
        Ok(result)
    }
    /// Whether the caller already runs on a room DSP worker.
    pub fn in_pool(&self) -> bool {
        self._pool.current_thread_index().is_some()
    }
    pub fn warm(&self) -> Result<()> {
        self.run(self.noop.clone())
    }
    pub fn run(&self, job: Arc<dyn Work>) -> Result<()> {
        // A canonical block job may invoke field/decoder jobs. They already own
        // their buffers and run inside this pool; another mailbox round trip
        // would deadlock the serial dispatcher and needlessly park its caller.
        if self._pool.current_thread_index().is_some() {
            return job.run();
        }
        let _profile = super::profile::Guard::new(super::profile::Stage::WorkerRoundtrip);
        let _serial = self.serial.lock().unwrap();
        let mut state = self.shared.state.lock().unwrap();
        state.result = None;
        state.job = Some(job);
        self.shared.changed.notify_all();
        while state.result.is_none() {
            state = self.shared.changed.wait(state).unwrap();
        }
        state.result.take().unwrap()
    }
    #[cfg(feature = "room-gpu-experiment")]
    pub(crate) fn run_overlapping<T>(
        &self,
        job: Arc<dyn Work>,
        prepare: impl FnOnce() -> Result<T>,
    ) -> Result<T> {
        if self.in_pool() {
            job.run()?;
            return prepare();
        }
        let _profile = super::profile::Guard::new(super::profile::Stage::WorkerRoundtrip);
        let _serial = self.serial.lock().unwrap();
        {
            let mut state = self.shared.state.lock().unwrap();
            state.result = None;
            state.job = Some(job);
            self.shared.changed.notify_all();
        }
        let mut pending = Pending {
            shared: &self.shared,
            finished: false,
        };
        let prepared = catch_unwind(AssertUnwindSafe(prepare));
        // Preserve execution-error precedence and never leave work in flight
        // when the caller receives either success or a preparation error.
        let executed = pending.finish();
        drop(pending);
        // Release the mailbox normally before propagating a preparation panic;
        // another session must not inherit a poisoned serial mutex.
        drop(_serial);
        executed?;
        match prepared {
            Ok(value) => value,
            Err(panic) => std::panic::resume_unwind(panic),
        }
    }
}
impl Drop for Workers {
    fn drop(&mut self) {
        let mut state = self.shared.state.lock().unwrap();
        state.stop = true;
        self.shared.changed.notify_all();
    }
}

#[cfg(all(test, feature = "room-gpu-experiment"))]
mod gpu_tests {
    use super::*;
    use std::sync::Barrier;
    struct Rendezvous(Arc<Barrier>);
    impl Work for Rendezvous {
        fn run(&self) -> Result<()> {
            self.0.wait();
            Ok(())
        }
    }
    #[test]
    fn overlapping_preparation_runs_while_work_is_pending_and_drains_on_error() {
        let workers = Workers::new().unwrap();
        let barrier = Arc::new(Barrier::new(2));
        let result: Result<()> =
            workers.run_overlapping(Arc::new(Rendezvous(barrier.clone())), || {
                barrier.wait();
                Err(Error::InvalidArgument("preparation rejected"))
            });
        assert!(matches!(
            result,
            Err(Error::InvalidArgument("preparation rejected"))
        ));
        workers.run(Arc::new(Noop)).unwrap();
        let panic = catch_unwind(AssertUnwindSafe(|| {
            let _: Result<()> = workers.run_overlapping(Arc::new(Noop), || panic!("prepare panic"));
        }));
        assert!(panic.is_err());
        workers.run(Arc::new(Noop)).unwrap();
    }
}
