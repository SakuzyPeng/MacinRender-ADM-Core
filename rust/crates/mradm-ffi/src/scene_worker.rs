//! Windows services of the Scene stream worker while a room renders (formerly C++
//! `room_worker_wait.cpp`): the thread's MMCSS "Audio" registration and a high-resolution
//! waitable timer for the output ring wait. Each thread owns its timer and registration; no
//! global timer resolution or priority changes and no handle creation during room DSP. Without
//! the timer the wait falls back to a sleep.
use std::cell::RefCell;
use std::ffi::c_void;
use std::time::Duration;

type Handle = *mut c_void;

const CREATE_WAITABLE_TIMER_HIGH_RESOLUTION: u32 = 0x0000_0002;
const TIMER_MODIFY_STATE: u32 = 0x0002;
const SYNCHRONIZE: u32 = 0x0010_0000;
const INFINITE: u32 = 0xFFFF_FFFF;
const WAIT_OBJECT_0: u32 = 0;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateWaitableTimerExW(
        attributes: *const c_void,
        name: *const u16,
        flags: u32,
        access: u32,
    ) -> Handle;
    fn SetWaitableTimer(
        timer: Handle,
        due: *const i64,
        period: i32,
        routine: *const c_void,
        argument: *const c_void,
        resume: i32,
    ) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
    fn CloseHandle(handle: Handle) -> i32;
}

#[link(name = "avrt")]
unsafe extern "system" {
    fn AvSetMmThreadCharacteristicsW(name: *const u16, index: *mut u32) -> Handle;
    fn AvRevertMmThreadCharacteristics(handle: Handle) -> i32;
}

struct Timer(Handle);

impl Timer {
    fn new() -> Self {
        Self(unsafe {
            CreateWaitableTimerExW(
                std::ptr::null(),
                std::ptr::null(),
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE,
            )
        })
    }

    fn wait(&self, duration: Duration) -> bool {
        if self.0.is_null() {
            return false;
        }
        let due = -i64::try_from(duration.as_millis()).unwrap_or(i64::MAX / 10_000) * 10_000;
        unsafe {
            SetWaitableTimer(self.0, &due, 0, std::ptr::null(), std::ptr::null(), 0) != 0
                && WaitForSingleObject(self.0, INFINITE) == WAIT_OBJECT_0
        }
    }
}

impl Drop for Timer {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { CloseHandle(self.0) };
        }
    }
}

#[derive(Default)]
struct Task {
    handle: Option<Handle>,
    active: bool,
}

impl Task {
    fn set_active(&mut self, active: bool) {
        if active == self.active {
            return;
        }
        self.active = active;
        if active {
            let mut index = 0;
            let name = [65u16, 117, 100, 105, 111, 0]; // Audio
            let handle = unsafe { AvSetMmThreadCharacteristicsW(name.as_ptr(), &mut index) };
            self.handle = (!handle.is_null()).then_some(handle);
        } else if let Some(handle) = self.handle.take() {
            unsafe { AvRevertMmThreadCharacteristics(handle) };
        }
    }
}

impl Drop for Task {
    fn drop(&mut self) {
        self.set_active(false);
    }
}

thread_local! {
    static TIMER: Timer = Timer::new();
    static TASK: RefCell<Task> = RefCell::new(Task::default());
}

pub(crate) fn wait(duration: Duration) {
    if TIMER.with(|timer| timer.wait(duration)) {
        return;
    }
    // Older Windows versions or exhausted kernel resources keep a non-spinning fallback.
    std::thread::sleep(duration);
}

pub(crate) fn set_active(active: bool) {
    TASK.with_borrow_mut(|task| task.set_active(active));
}
