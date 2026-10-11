//! Room-only floating-point environment boundary. Dedicated DSP threads inherit
//! this environment when preparation starts them; caller settings are restored.
pub struct Environment {
    #[cfg(target_arch = "x86_64")]
    control: u32,
    #[cfg(target_arch = "aarch64")]
    control: u64,
    #[cfg(target_arch = "aarch64")]
    status: u64,
}

#[cfg(windows)]
mod audio_worker {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static REGISTERED: AtomicUsize = AtomicUsize::new(0);
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
            let name = [65, 117, 100, 105, 111, 0]; // Audio
            let handle = unsafe { AvSetMmThreadCharacteristicsW(name.as_ptr(), &mut index) };
            if !handle.is_null() {
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
    pub fn registered() -> usize {
        REGISTERED.load(Ordering::Relaxed)
    }
}

#[cfg(windows)]
pub use audio_worker::start as start_worker;
pub fn mmcss_workers() -> usize {
    #[cfg(windows)]
    {
        audio_worker::registered()
    }
    #[cfg(not(windows))]
    {
        0
    }
}

impl Environment {
    pub fn enter() -> Self {
        #[cfg(target_arch = "x86_64")]
        unsafe {
            let mut control = 0u32;
            core::arch::asm!("stmxcsr [{p}]",p=in(reg)&mut control,options(nostack,preserves_flags));
            // RNE; retain gradual underflow; mask exceptions for checked error handling.
            let canonical = (control & !((1 << 15) | (1 << 6) | (3 << 13))) | (0x3f << 7);
            core::arch::asm!("ldmxcsr [{p}]",p=in(reg)&canonical,options(nostack,preserves_flags));
            Self { control }
        }
        #[cfg(target_arch = "aarch64")]
        unsafe {
            let control: u64;
            let status: u64;
            core::arch::asm!("mrs {v}, fpcr",v=out(reg)control,options(nostack,preserves_flags));
            core::arch::asm!("mrs {v}, fpsr",v=out(reg)status,options(nostack,preserves_flags));
            // RNE, FZ/FZ16 off, exception traps off. FPCR is per thread.
            let canonical =
                control & !((3 << 22) | (1 << 24) | (1 << 19) | (0x1f << 8) | (1 << 15));
            core::arch::asm!("msr fpcr, {v}",v=in(reg)canonical,options(nostack,preserves_flags));
            Self { control, status }
        }
        #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
        Self {}
    }
}
impl Drop for Environment {
    fn drop(&mut self) {
        #[cfg(target_arch = "x86_64")]
        unsafe {
            core::arch::asm!("ldmxcsr [{p}]",p=in(reg)&self.control,options(nostack,preserves_flags));
        }
        #[cfg(target_arch = "aarch64")]
        unsafe {
            core::arch::asm!("msr fpcr, {v}",v=in(reg)self.control,options(nostack,preserves_flags));
            core::arch::asm!("msr fpsr, {v}",v=in(reg)self.status,options(nostack,preserves_flags));
        }
    }
}

#[cfg(all(test, any(target_arch = "x86_64", target_arch = "aarch64")))]
mod tests {
    use super::Environment;
    #[test]
    fn room_normalizes_and_restores_the_callers_environment() {
        let _restore = Environment::enter();
        let mut expected = [0f32; 3];
        let mut actual = [0f32; 3];
        unsafe {
            assert_eq!(
                crate::room::mradm_room_polar(41.25, 15.75, 1.0e-38, expected.as_mut_ptr()),
                0
            );
            #[cfg(target_arch = "x86_64")]
            let changed = {
                let mut value = 0u32;
                core::arch::asm!("stmxcsr [{p}]",p=in(reg)&mut value,options(nostack,preserves_flags));
                value |= (1 << 15) | (1 << 6) | (2 << 13);
                core::arch::asm!("ldmxcsr [{p}]",p=in(reg)&value,options(nostack,preserves_flags));
                value as u64
            };
            #[cfg(target_arch = "aarch64")]
            let changed = {
                let mut value: u64;
                core::arch::asm!("mrs {v}, fpcr",v=out(reg)value,options(nostack,preserves_flags));
                value |= (1 << 24) | (1 << 22);
                core::arch::asm!("msr fpcr, {v}",v=in(reg)value,options(nostack,preserves_flags));
                value
            };
            assert_eq!(
                crate::room::mradm_room_polar(41.25, 15.75, 1.0e-38, actual.as_mut_ptr()),
                0
            );
            assert_eq!(expected.map(f32::to_bits), actual.map(f32::to_bits));
            #[cfg(target_arch = "x86_64")]
            let restored = {
                let mut value = 0u32;
                core::arch::asm!("stmxcsr [{p}]",p=in(reg)&mut value,options(nostack,preserves_flags));
                value as u64
            };
            #[cfg(target_arch = "aarch64")]
            let restored = {
                let value: u64;
                core::arch::asm!("mrs {v}, fpcr",v=out(reg)value,options(nostack,preserves_flags));
                value
            };
            assert_eq!(restored, changed);
        }
    }
}
