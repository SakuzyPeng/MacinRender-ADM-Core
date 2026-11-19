//! Shared by the manual benchmarks and probes (formerly C++ under `tests/manual`): the process's
//! resident memory and a null output clock.
#![allow(dead_code)]
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

/// The resident set in bytes (0 when unavailable): the Mach task's resident size on macOS, the
/// working set on Windows, `/proc/self/statm` elsewhere.
pub fn rss() -> u64 {
    platform::rss()
}

/// The peak resident set in bytes (`getrusage`'s `ru_maxrss`, which macOS reports in bytes); 0
/// elsewhere.
pub fn peak_rss() -> u64 {
    platform::peak_rss()
}

#[cfg(target_os = "macos")]
mod platform {
    #[repr(C)]
    #[derive(Default)]
    struct TimeValue {
        seconds: i32,
        microseconds: i32,
    }
    /// `mach_task_basic_info`.
    #[repr(C)]
    #[derive(Default)]
    struct TaskBasicInfo {
        virtual_size: u64,
        resident_size: u64,
        resident_size_max: u64,
        user_time: TimeValue,
        system_time: TimeValue,
        policy: i32,
        suspend_count: i32,
    }
    #[repr(C)]
    #[derive(Default)]
    struct Timeval {
        seconds: i64,
        microseconds: i32,
    }
    /// `struct rusage`: two `timeval`s and fourteen `long`s.
    #[repr(C)]
    #[derive(Default)]
    struct Rusage {
        user: Timeval,
        system: Timeval,
        longs: [i64; 14],
    }
    const MACH_TASK_BASIC_INFO: i32 = 20;
    unsafe extern "C" {
        static mach_task_self_: u32;
        fn task_info(task: u32, flavor: i32, info: *mut i32, count: *mut u32) -> i32;
        fn getrusage(who: i32, usage: *mut Rusage) -> i32;
    }
    pub fn rss() -> u64 {
        let mut info = TaskBasicInfo::default();
        let mut count = (size_of::<TaskBasicInfo>() / size_of::<i32>()) as u32;
        // SAFETY: `info` holds `count` naturals of the flavour's record.
        let status = unsafe {
            task_info(
                mach_task_self_,
                MACH_TASK_BASIC_INFO,
                (&raw mut info).cast(),
                &mut count,
            )
        };
        if status == 0 { info.resident_size } else { 0 }
    }
    pub fn peak_rss() -> u64 {
        let mut usage = Rusage::default();
        // SAFETY: RUSAGE_SELF (0) into a `struct rusage`; `ru_maxrss` is the first long.
        if unsafe { getrusage(0, &mut usage) } == 0 {
            usage.longs[0] as u64
        } else {
            0
        }
    }
}

#[cfg(windows)]
mod platform {
    /// `PROCESS_MEMORY_COUNTERS`.
    #[repr(C)]
    #[derive(Default)]
    struct Counters {
        cb: u32,
        page_fault_count: u32,
        peak_working_set_size: usize,
        working_set_size: usize,
        quota_peak_paged_pool_usage: usize,
        quota_paged_pool_usage: usize,
        quota_peak_non_paged_pool_usage: usize,
        quota_non_paged_pool_usage: usize,
        pagefile_usage: usize,
        peak_pagefile_usage: usize,
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentProcess() -> *mut core::ffi::c_void;
        fn K32GetProcessMemoryInfo(
            process: *mut core::ffi::c_void,
            counters: *mut Counters,
            cb: u32,
        ) -> i32;
    }
    pub fn rss() -> u64 {
        let mut counters = Counters {
            cb: size_of::<Counters>() as u32,
            ..Counters::default()
        };
        // SAFETY: the pseudo handle of this process and a sized counters record.
        if unsafe { K32GetProcessMemoryInfo(GetCurrentProcess(), &mut counters, counters.cb) } != 0
        {
            counters.working_set_size as u64
        } else {
            0
        }
    }
    pub fn peak_rss() -> u64 {
        0
    }
}

#[cfg(not(any(target_os = "macos", windows)))]
mod platform {
    /// The page size of the targets this runs on; `statm` counts pages.
    const PAGE: u64 = 4096;
    pub fn rss() -> u64 {
        std::fs::read_to_string("/proc/self/statm")
            .ok()
            .and_then(|s| s.split_whitespace().nth(1)?.parse::<u64>().ok())
            .map_or(0, |pages| pages * PAGE)
    }
    pub fn peak_rss() -> u64 {
        0
    }
}

/// A null output device: a thread that pulls `period` frames every period of the sample clock
/// while playing, like miniaudio's null backend (10 ms periods), and drops the PCM.
pub struct NullClock {
    playing: AtomicBool,
    stop: AtomicBool,
}

impl Default for NullClock {
    fn default() -> Self {
        Self {
            playing: AtomicBool::new(false),
            stop: AtomicBool::new(false),
        }
    }
}

impl NullClock {
    pub fn pause(&self) {
        self.playing.store(false, Ordering::Release);
    }
    pub fn resume(&self) {
        self.playing.store(true, Ordering::Release);
    }
    pub fn stop(&self) {
        self.stop.store(true, Ordering::Release);
    }
    /// Runs on the calling thread until [`Self::stop`]: `pull(buffer, frames)` once per period.
    pub fn run(&self, channels: usize, sample_rate: u32, mut pull: impl FnMut(&mut [f32], usize)) {
        let frames = sample_rate as usize / 100;
        let period = Duration::from_secs_f64(frames as f64 / f64::from(sample_rate));
        let mut buffer = vec![0f32; frames * channels];
        let mut next = Instant::now();
        while !self.stop.load(Ordering::Acquire) {
            next += period;
            if let Some(wait) = next.checked_duration_since(Instant::now()) {
                std::thread::sleep(wait);
            } else {
                // Late: keep the clock instead of bursting to catch up.
                next = Instant::now();
            }
            if self.playing.load(Ordering::Acquire) {
                pull(&mut buffer, frames);
            }
        }
    }
}
