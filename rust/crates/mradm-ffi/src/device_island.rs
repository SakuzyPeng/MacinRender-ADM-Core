//! The output devices (`src/adm_realtime/device_island.h`, `MradmDeviceIslands`): miniaudio, its
//! null backend and the platform's system-spatial sink, opened and driven from Rust for the
//! monitor session and the Scene output.
use super::engine_monitor::{Buffers, EngineIslandError};
use super::engine_scene_output::EngineSceneDeviceProgress;
use crate::adm::Text;
use mradm_engine::scene::output::DeviceProgress;
use mradm_render::routing::Error;
use std::ffi::c_void;

/// A device's pull: fills `out` (`len` floats) with up to `frames` frames on its audio thread.
pub(crate) type Pull = unsafe extern "C" fn(*mut c_void, *mut f32, usize, usize) -> usize;

/// A device's pull and its user data (copied by `start`).
#[repr(C)]
pub struct DevicePull {
    user: *mut c_void,
    pull: Option<unsafe extern "C" fn(*mut c_void, *mut f32, usize, usize) -> usize>,
}

/// One playback device per `each` call (`enumerate`).
#[repr(C)]
pub struct DeviceEnumeration {
    user: *mut c_void,
    each: Option<unsafe extern "C" fn(*mut c_void, Text, Text, u32)>,
}

/// An opened output device (`IAudioOutputDevice`); `destroy` releases it after `stop`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DeviceTable {
    ctx: *mut c_void,
    start: Option<
        unsafe extern "C" fn(
            *mut c_void,
            u32,
            u32,
            *const DevicePull,
            *const EngineIslandError,
        ) -> i32,
    >,
    stop: Option<unsafe extern "C" fn(*mut c_void)>,
    flush: Option<unsafe extern "C" fn(*mut c_void)>,
    pause: Option<unsafe extern "C" fn(*mut c_void)>,
    resume: Option<unsafe extern "C" fn(*mut c_void)>,
    mark_end: Option<unsafe extern "C" fn(*mut c_void)>,
    realtime: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    has_volume: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    set_volume: Option<unsafe extern "C" fn(*mut c_void, f32)>,
    progress: Option<unsafe extern "C" fn(*mut c_void, *mut EngineSceneDeviceProgress)>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

/// The platform's output devices. `open` kind: 0 the system-spatial sink, 1 the hardware device,
/// 2 the null backend; `system_spatial` is 1 where the platform has that sink.
#[repr(C)]
pub struct DeviceIslands {
    system_spatial: u32,
    open: Option<
        unsafe extern "C" fn(
            u32,
            Text,
            u32,
            Text,
            *mut DeviceTable,
            *const EngineIslandError,
        ) -> i32,
    >,
    enumerate: Option<unsafe extern "C" fn(*const DeviceEnumeration)>,
}

/// `open` kinds.
pub(crate) const SYSTEM_SPATIAL: u32 = 0;
pub(crate) const HARDWARE: u32 = 1;
pub(crate) const NULL_BACKEND: u32 = 2;

/// A caller's device table (static in the shell).
#[derive(Clone, Copy)]
pub(crate) struct Devices(*const DeviceIslands);
// SAFETY: The table is immutable process data and its entries are thread-safe.
unsafe impl Send for Devices {}
unsafe impl Sync for Devices {}

impl Devices {
    pub(crate) fn new(table: *const DeviceIslands) -> Option<Self> {
        (!table.is_null()).then_some(Self(table))
    }

    fn table(&self) -> &DeviceIslands {
        unsafe { &*self.0 }
    }

    /// Whether the platform has a system-spatial sink.
    pub(crate) fn system_spatial(&self) -> bool {
        self.table().system_spatial != 0
    }

    /// Opens a device of `kind`.
    pub(crate) fn open(
        &self,
        kind: u32,
        layout: &str,
        geometry: u32,
        device_id: &[u8],
    ) -> Result<Device, Error> {
        let Some(open) = self.table().open else {
            return Err(Error {
                code: 6,
                message: b"output devices are unavailable".to_vec(),
                context: Vec::new(),
            });
        };
        let mut table = DeviceTable::empty();
        Buffers::new().call(|e| unsafe {
            open(
                kind,
                Text::new(layout),
                geometry,
                Text::bytes(device_id),
                &mut table,
                e,
            )
        })?;
        Ok(Device { table })
    }
}

impl DeviceTable {
    fn empty() -> Self {
        Self {
            ctx: std::ptr::null_mut(),
            start: None,
            stop: None,
            flush: None,
            pause: None,
            resume: None,
            mark_end: None,
            realtime: None,
            has_volume: None,
            set_volume: None,
            progress: None,
            destroy: None,
        }
    }
}

/// An opened device; dropping it destroys the device (callers stop it first).
pub(crate) struct Device {
    table: DeviceTable,
}
// SAFETY: An `IAudioOutputDevice`, controlled from the worker, the control thread and its own
// callback as before.
unsafe impl Send for Device {}
unsafe impl Sync for Device {}

impl Device {
    /// Takes over a caller's table (a C++ device wrapped by `device_table`).
    pub(crate) unsafe fn adopt(table: &DeviceTable) -> Self {
        Self { table: *table }
    }

    fn call(&self, f: Option<unsafe extern "C" fn(*mut c_void)>) {
        if let Some(f) = f {
            unsafe { f(self.table.ctx) };
        }
    }

    /// Starts pulling with `pull(user, …)`; `user` must outlive the device's callbacks (until
    /// `stop`). Code 100: the device threw.
    pub(crate) fn start(
        &self,
        channels: u32,
        sample_rate: u32,
        user: *mut c_void,
        pull: Pull,
    ) -> Result<(), Error> {
        let Some(start) = self.table.start else {
            return Ok(());
        };
        let ctx = self.table.ctx;
        let pull = DevicePull {
            user,
            pull: Some(pull),
        };
        Buffers::new().call(|e| unsafe { start(ctx, channels, sample_rate, &pull, e) })
    }

    pub(crate) fn stop(&self) {
        self.call(self.table.stop);
    }
    pub(crate) fn flush(&self) {
        self.call(self.table.flush);
    }
    pub(crate) fn pause(&self) {
        self.call(self.table.pause);
    }
    pub(crate) fn resume(&self) {
        self.call(self.table.resume);
    }
    /// From inside the device's own pull.
    pub(crate) fn mark_end(&self) {
        self.call(self.table.mark_end);
    }

    /// `pull_is_realtime_playback`.
    pub(crate) fn realtime(&self) -> bool {
        self.table
            .realtime
            .is_some_and(|f| unsafe { f(self.table.ctx) } != 0)
    }

    pub(crate) fn has_volume(&self) -> bool {
        self.table
            .has_volume
            .is_some_and(|f| unsafe { f(self.table.ctx) } != 0)
    }

    pub(crate) fn set_volume(&self, gain: f32) {
        if let Some(f) = self.table.set_volume {
            unsafe { f(self.table.ctx, gain) };
        }
    }

    pub(crate) fn progress(&self) -> DeviceProgress {
        let mut p = EngineSceneDeviceProgress {
            presented_frames: 0,
            media_clock: 0,
            failed: 0,
            recovering: 0,
            underruns: 0,
        };
        if let Some(f) = self.table.progress {
            unsafe { f(self.table.ctx, &mut p) };
        }
        DeviceProgress {
            presented_frames: p.presented_frames,
            media_clock: p.media_clock != 0,
            failed: p.failed != 0,
            recovering: p.recovering != 0,
            underruns: p.underruns,
        }
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        self.call(self.table.destroy);
    }
}

/// The interleaved output buffer of one pull; a hostile length aborts as before.
pub(crate) unsafe fn output<'a>(out: *mut f32, len: usize) -> &'a mut [f32] {
    if len == 0 {
        return &mut [];
    }
    if out.is_null() || len > isize::MAX as usize / 4 {
        std::process::abort();
    }
    unsafe { std::slice::from_raw_parts_mut(out, len) }
}

#[cfg(test)]
impl DeviceIslands {
    pub(crate) fn for_test(
        system_spatial: u32,
        open: Option<
            unsafe extern "C" fn(
                u32,
                Text,
                u32,
                Text,
                *mut DeviceTable,
                *const EngineIslandError,
            ) -> i32,
        >,
    ) -> Self {
        Self {
            system_spatial,
            open,
            enumerate: None,
        }
    }
}
