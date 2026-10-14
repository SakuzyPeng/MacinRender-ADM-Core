use crate::sync::{Condvar, Mutex};

/// The sleep state for an individual worker.
#[cfg_attr(not(target_vendor = "apple"), derive(Default))]
pub(super) struct WorkerSleepState {
    /// Set when the worker goes to sleep and cleared when it is notified or wakes.
    pub(super) is_blocked: Mutex<bool>,
    pub(super) condvar: Condvar,
}

#[cfg(target_vendor = "apple")]
impl Default for WorkerSleepState {
    fn default() -> Self {
        let state = Self {
            is_blocked: Mutex::new(false),
            condvar: Condvar::new(),
        };
        // MacinRender: Apple's pthread-backed std locks allocate lazily. Prepare
        // the actual worker parking pair before the pool publishes any threads.
        // A zero-duration wait initializes both objects without a scheduling
        // handshake or depending on a worker reaching the sleep path in time.
        drop(
            state
                .condvar
                .wait_timeout(state.is_blocked.lock().unwrap(), std::time::Duration::ZERO)
                .unwrap(),
        );
        state
    }
}
