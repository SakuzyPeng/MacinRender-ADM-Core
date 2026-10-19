//! Optional benchmark alignment with the production FFI's Windows Audio class.
#[cfg(windows)]
mod native {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static REGISTERED: AtomicUsize = AtomicUsize::new(0);
    static FAILED: AtomicUsize = AtomicUsize::new(0);
    #[link(name = "avrt")]
    unsafe extern "system" {
        fn AvSetMmThreadCharacteristicsW(
            name: *const u16,
            index: *mut u32,
        ) -> *mut core::ffi::c_void;
        fn AvRevertMmThreadCharacteristics(handle: *mut core::ffi::c_void) -> i32;
    }
    struct Task(*mut core::ffi::c_void);
    impl Task {
        fn new() -> Self {
            let mut index = 0;
            let name = [65, 117, 100, 105, 111, 0];
            let handle = unsafe { AvSetMmThreadCharacteristicsW(name.as_ptr(), &mut index) };
            if handle.is_null() {
                FAILED.fetch_add(1, Ordering::Relaxed);
            } else {
                REGISTERED.fetch_add(1, Ordering::Relaxed);
            }
            Self(handle)
        }
    }
    impl Drop for Task {
        fn drop(&mut self) {
            if !self.0.is_null() {
                unsafe {
                    AvRevertMmThreadCharacteristics(self.0);
                }
                REGISTERED.fetch_sub(1, Ordering::Relaxed);
            }
        }
    }
    pub fn start() {
        thread_local! { static TASK: Task = Task::new(); }
        TASK.with(|_| {});
    }
    pub fn report() -> Result<serde_json::Value, Box<dyn std::error::Error>> {
        let workers = REGISTERED.load(Ordering::Relaxed);
        let expected = std::env::var("MR_ADM_ROOM_WORKERS")
            .ok()
            .map(|v| v.parse::<usize>())
            .transpose()?
            .unwrap_or_else(|| {
                std::thread::available_parallelism()
                    .map_or(1, usize::from)
                    .min(8)
            });
        if FAILED.load(Ordering::Relaxed) != 0 || workers != expected {
            return Err("Windows Audio worker registration incomplete".into());
        }
        Ok(
            serde_json::json!({"class":"Audio","registered_workers":workers,"caller_registered":false}),
        )
    }
}
pub fn enable() -> Result<(), Box<dyn std::error::Error>> {
    #[cfg(windows)]
    {
        mradm_dsp::room::set_worker_start_hook(native::start);
        Ok(())
    }
    #[cfg(not(windows))]
    {
        Err("--audio-workers requires Windows".into())
    }
}
pub fn report() -> Result<serde_json::Value, Box<dyn std::error::Error>> {
    #[cfg(windows)]
    {
        native::report()
    }
    #[cfg(not(windows))]
    {
        Err("--audio-workers requires Windows".into())
    }
}
