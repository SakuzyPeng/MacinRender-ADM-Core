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
pub struct Workers {
    shared: Arc<Shared>,
    serial: Mutex<()>,
    _pool: rayon::ThreadPool,
    noop: Arc<Noop>,
}
impl Workers {
    pub fn new() -> Result<Self> {
        let default_count = std::thread::available_parallelism()
            .map_or(1, usize::from)
            .min(8);
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
        // Start every thread while allocations are permitted.
        pool.broadcast(|_| {
            (0..512usize).into_par_iter().for_each(|i| {
                std::hint::black_box(i);
            });
        });
        if count > 1 {
            // Rayon parks a joining worker on its private condvar. macOS's
            // pthread backing is lazy, so exercise that path before playback.
            for index in 0..count {
                pool.broadcast(|context| {
                    if context.index() != index {
                        return;
                    }
                    let started = std::sync::atomic::AtomicBool::new(false);
                    rayon::join(
                        || {
                            while !started.load(std::sync::atomic::Ordering::Acquire) {
                                std::thread::yield_now();
                            }
                        },
                        || {
                            started.store(true, std::sync::atomic::Ordering::Release);
                            std::thread::sleep(std::time::Duration::from_millis(5));
                        },
                    );
                });
            }
        }
        let shared = Arc::new(Shared {
            state: Mutex::new(Mailbox {
                job: None,
                result: None,
                stop: false,
            }),
            changed: Condvar::new(),
        });
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
    pub fn warm(&self) -> Result<()> {
        self.run(self.noop.clone())
    }
    pub fn run(&self, job: Arc<dyn Work>) -> Result<()> {
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
}
impl Drop for Workers {
    fn drop(&mut self) {
        let mut state = self.shared.state.lock().unwrap();
        state.stop = true;
        self.shared.changed.notify_all();
    }
}
