//! The calling thread's MMCSS "Audio" task. AVRT requires reversion on that same thread.
use windows::Win32::{
    Foundation::HANDLE,
    System::Threading::{AvRevertMmThreadCharacteristics, AvSetMmThreadCharacteristicsW},
};
use windows::core::w;

pub(crate) struct AudioTask(HANDLE);

impl AudioTask {
    pub(crate) fn register() -> Option<Self> {
        unsafe { AvSetMmThreadCharacteristicsW(w!("Audio"), &mut 0) }
            .ok()
            .map(Self)
    }
}

impl Drop for AudioTask {
    fn drop(&mut self) {
        let _ = unsafe { AvRevertMmThreadCharacteristics(self.0) };
    }
}

#[cfg(test)]
mod tests {
    #[test]
    #[ignore = "requires the Windows MMCSS service"]
    fn room_worker_registration_lasts_until_thread_exit() {
        use crate::room_environment::{mmcss_workers, start_worker};
        let before = mmcss_workers();
        std::thread::spawn(move || {
            start_worker();
            start_worker();
            assert_eq!(mmcss_workers(), before + 1);
        })
        .join()
        .unwrap();
        assert_eq!(mmcss_workers(), before);
    }
}
