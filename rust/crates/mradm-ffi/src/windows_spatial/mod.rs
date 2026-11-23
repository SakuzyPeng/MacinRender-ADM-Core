//! Event-driven Windows system-spatial playback for file and Scene streaming.
mod bed;
mod stream;

use crate::render::{
    device_island::{DevicePull, DeviceTable},
    engine_monitor::{EngineIslandError, island_error_context},
    engine_scene_output::EngineSceneDeviceProgress,
};
use bed::Bed;
pub(crate) use bed::layouts;
use mradm_engine::request::{Error, code};
use mradm_scene::layouts::Geometry;
use std::{
    ffi::c_void,
    os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use stream::{Apartment, RENDER_FAILED, Stream};
use windows::Win32::{
    Foundation::HANDLE,
    System::Threading::{CreateEventW, SetEvent, WaitForSingleObject},
};

struct Wake {
    event: OwnedHandle,
    stopping: AtomicBool,
}
impl Wake {
    fn new() -> Result<Self, Error> {
        // Auto-reset. It outlives stream rebuilds and the final callback.
        let handle = unsafe { CreateEventW(None, false, false, None) }
            .map_err(|_| Error::new(RENDER_FAILED, "创建空间音频缓冲事件失败", "system-spatial"))?;
        Ok(Self {
            event: unsafe { OwnedHandle::from_raw_handle(handle.0) },
            stopping: AtomicBool::new(false),
        })
    }

    fn handle(&self) -> HANDLE {
        HANDLE(self.event.as_raw_handle())
    }

    fn stopped(&self) -> bool {
        self.stopping.load(Ordering::Acquire)
    }
}

struct Running {
    wake: Arc<Wake>,
    thread: JoinHandle<()>,
}

impl Running {
    fn stop(self) {
        self.wake.stopping.store(true, Ordering::Release);
        let _ = unsafe { SetEvent(self.wake.handle()) };
        self.thread.thread().unpark(); // also wake a spatializer-recovery backoff
        let _ = self.thread.join();
    }
}

// SAFETY: the engine grants its pull and context to one audio thread. stop joins
// that thread before the engine can release the context. COM pointers never cross threads.
struct ThreadPull(DevicePull);
unsafe impl Send for ThreadPull {}

impl ThreadPull {
    fn read(&self, out: &mut [f32], frames: usize) -> usize {
        self.0.pull.map_or(0, |f| unsafe {
            f(self.0.user, out.as_mut_ptr(), out.len(), frames, 1)
        })
    }
}

fn worker(
    bed: Bed,
    rate: u32,
    pull: ThreadPull,
    wake: &Wake,
    recovering: &AtomicBool,
    ready: mpsc::Sender<Result<(), Error>>,
) {
    let setup = Apartment::enter().and_then(|apartment| {
        Stream::open(&bed, rate, wake.handle()).map(|stream| (apartment, stream))
    });
    let (_apartment, stream) = match setup {
        Ok(pair) => pair,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    if ready.send(Ok(())).is_err() {
        return;
    }
    let mut read = |out: &mut [f32], frames| pull.read(out, frames);
    pump(
        stream,
        wake,
        recovering,
        || Stream::open(&bed, rate, wake.handle()),
        |stream| stream.update(&bed, &mut read),
    );
}

// Retry on the same thread to keep every stream in its original COM apartment.
fn pump<S>(
    mut stream: S,
    wake: &Wake,
    recovering: &AtomicBool,
    mut reopen: impl FnMut() -> Result<S, Error>,
    mut update: impl FnMut(&mut S) -> windows::core::Result<()>,
) {
    loop {
        while !wake.stopped() {
            // Also probe after timeout: an invalidated spatializer can stop signaling.
            unsafe { WaitForSingleObject(wake.handle(), 100) };
            if wake.stopped() {
                return;
            }
            if update(&mut stream).is_err() {
                break;
            }
        }
        if wake.stopped() {
            return;
        }
        recovering.store(true, Ordering::Release);
        drop(stream);
        thread::park_timeout(Duration::from_millis(100));
        loop {
            if wake.stopped() {
                return;
            }
            if let Ok(rebuilt) = reopen() {
                stream = rebuilt;
                break;
            }
            thread::park_timeout(Duration::from_millis(150));
        }
        recovering.store(false, Ordering::Release);
    }
}

struct Device {
    layout: String,
    geometry: Geometry,
    recovering: Arc<AtomicBool>,
    running: Mutex<Option<Running>>,
}

impl Device {
    fn start(&self, channels: u32, rate: u32, pull: DevicePull) -> Result<(), Error> {
        let mut running = self.running.lock().unwrap_or_else(|e| e.into_inner());
        if running.is_some() {
            return Err(Error::new(
                code::INVALID_ARGUMENT,
                "audio device already started",
                "system-spatial",
            ));
        }
        let bed = Bed::new(&self.layout, channels, self.geometry)?;
        let wake = Arc::new(Wake::new()?);
        let (ready, result) = mpsc::channel();
        let pull = ThreadPull(pull);
        let worker_wake = Arc::clone(&wake);
        let recovering = Arc::clone(&self.recovering);
        let thread = thread::Builder::new()
            .name("windows-spatial".into())
            .spawn(move || worker(bed, rate, pull, &worker_wake, &recovering, ready))
            .map_err(|e| Error::new(RENDER_FAILED, "创建空间音频线程失败", e.to_string()))?;
        let setup = result.recv().unwrap_or_else(|_| {
            Err(Error::new(
                RENDER_FAILED,
                "空间音频线程初始化失败",
                "system-spatial",
            ))
        });
        if setup.is_ok() {
            *running = Some(Running { wake, thread });
        } else {
            let _ = thread.join();
        }
        setup
    }

    fn stop(&self) {
        let mut running = self.running.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(running) = running.take() {
            running.stop();
        }
        self.recovering.store(false, Ordering::Release);
    }
}

unsafe extern "C" fn start(
    ctx: *mut c_void,
    channels: u32,
    rate: u32,
    pull: *const DevicePull,
    e: *const EngineIslandError,
) -> i32 {
    let device = unsafe { &*ctx.cast::<Device>() };
    match device.start(channels, rate, unsafe { *pull }) {
        Ok(()) => 0,
        Err(error) => unsafe {
            island_error_context(e, error.code, &error.message, &error.context)
        },
    }
}
unsafe extern "C" fn stop(ctx: *mut c_void) {
    unsafe { &*ctx.cast::<Device>() }.stop();
}
unsafe extern "C" fn realtime(_: *mut c_void) -> u32 {
    1
}
unsafe extern "C" fn progress(ctx: *mut c_void, out: *mut EngineSceneDeviceProgress) {
    let device = unsafe { &*ctx.cast::<Device>() };
    unsafe {
        *out = EngineSceneDeviceProgress {
            presented_frames: 0,
            media_clock: 0,
            failed: 0,
            recovering: u32::from(device.recovering.load(Ordering::Acquire)),
            using_fallback: 0,
            underruns: 0,
        };
    }
}
unsafe extern "C" fn destroy(ctx: *mut c_void) {
    let device = unsafe { Box::from_raw(ctx.cast::<Device>()) };
    device.stop();
}

pub(crate) fn open(layout: String, geometry: u32) -> DeviceTable {
    let device = Device {
        layout,
        geometry: if geometry == 1 {
            Geometry::Apple
        } else {
            Geometry::Standard
        },
        recovering: Arc::new(AtomicBool::new(false)),
        running: Mutex::new(None),
    };
    DeviceTable {
        ctx: Box::into_raw(Box::new(device)).cast(),
        start: Some(start),
        stop: Some(stop),
        realtime: Some(realtime),
        progress: Some(progress),
        destroy: Some(destroy),
        ..DeviceTable::empty()
    }
}

#[cfg(test)]
mod tests;
