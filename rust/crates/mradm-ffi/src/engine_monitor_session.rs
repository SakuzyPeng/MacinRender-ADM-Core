//! The monitor session (`mradm-engine` `monitor::session`, PCM consistency 5.5 B) behind the C++
//! `MonitorSession` shell. The shell provides the devices, the Apple island and the backend
//! options through the host table below; its device callbacks pull the session's monitor with
//! [`mradm_engine_session_monitor_pull`].
use super::engine_monitor::{
    Buffers, EngineIslandError, EngineIslandStream, EngineLiveMonitorLevels, EngineLiveMonitorPull,
    EngineLiveMonitorStatus, IslandSource, overrides_of,
};
use super::room_ffi::{RenderRoomControl, control_handle};
use super::*;
use crate::engine::{EnginePlatform, platform};
use crate::engine_render::FfiHost;
use crate::engine_service::{EngineRenderRequest, request_of};
use crate::hptf::Snapshot;
use crate::hptf::settings::Settings;
use crate::scene::{DirectMatrix, DirectMatrixView, SceneHandle};
use mradm_engine::backend;
use mradm_engine::monitor::session::{
    Choice, Device, Entry, HptfDesign, Session, SessionHost, Token,
};
use mradm_engine::monitor::{Monitor, Source};
use mradm_scene::{RenderScene, direct_matrix};
use std::ffi::c_void;
use std::sync::{Arc, Mutex};

/// The shell's devices, Apple island and options. `options` are the caller's `RenderOptions`
/// copies the session holds until `release_options`; `device` handles until `device_destroy`.
#[repr(C)]
pub struct EngineMonitorSessionHost {
    ctx: *mut c_void,
    device: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, *const u8, usize) -> *mut c_void>,
    device_realtime: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> u32>,
    device_start: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut c_void,
            u32,
            u32,
            *const EngineSessionMonitor,
            *const EngineIslandError,
        ) -> i32,
    >,
    device_stop: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    device_flush: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    device_pause: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    device_resume: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    device_destroy: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    open_apple: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut c_void,
            *const u8,
            usize,
            *const DirectMatrixView,
            *mut EngineIslandStream,
            *const EngineIslandError,
        ) -> i32,
    >,
    release_options: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
}

/// A session's monitor as its device callback sees it: an opaque `Monitor` pointer.
pub struct EngineSessionMonitor {
    _opaque: [u8; 0],
}

pub struct EngineMonitorSession {
    session: Session,
    table: Arc<Table>,
    entry: Mutex<Option<Entry>>,
}

struct Table(EngineMonitorSessionHost);
// SAFETY: The shell's host callbacks are thread-safe; its context outlives the session.
unsafe impl Send for Table {}
unsafe impl Sync for Table {}

/// The caller's options of one choice.
struct Options {
    table: Arc<Table>,
    ctx: *mut c_void,
}
// SAFETY: The options are an immutable C++ copy, released once.
unsafe impl Send for Options {}
unsafe impl Sync for Options {}

impl Drop for Options {
    fn drop(&mut self) {
        if let Some(release) = self.table.0.release_options {
            unsafe { release(self.table.0.ctx, self.ctx) };
        }
    }
}

fn options_ctx(token: &Token) -> *mut c_void {
    token
        .downcast_ref::<Options>()
        .map_or(ptr::null_mut(), |o| o.ctx)
}

struct ShellDevice {
    table: Arc<Table>,
    device: *mut c_void,
}
// SAFETY: An `IAudioOutputDevice`, controlled from the worker and the control thread as before.
unsafe impl Send for ShellDevice {}
unsafe impl Sync for ShellDevice {}

impl ShellDevice {
    fn call(&self, f: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>) {
        if let Some(f) = f {
            unsafe { f(self.table.0.ctx, self.device) };
        }
    }
}

impl Drop for ShellDevice {
    fn drop(&mut self) {
        self.call(self.table.0.device_destroy);
    }
}

impl Device for ShellDevice {
    fn realtime(&self) -> bool {
        self.table
            .0
            .device_realtime
            .is_some_and(|f| unsafe { f(self.table.0.ctx, self.device) } != 0)
    }
    fn start(&self, channels: usize, sample_rate: u32, monitor: &Monitor) -> Result<(), Error> {
        let Some(f) = self.table.0.device_start else {
            return Ok(());
        };
        // SAFETY of the pointer: the session stops this device before the monitor drops.
        let pulled = (monitor as *const Monitor).cast::<EngineSessionMonitor>();
        let (ctx, device) = (self.table.0.ctx, self.device);
        Buffers::new().call(|e| unsafe { f(ctx, device, channels as u32, sample_rate, pulled, e) })
    }
    fn stop(&self) {
        self.call(self.table.0.device_stop);
    }
    fn flush(&self) {
        self.call(self.table.0.device_flush);
    }
    fn pause(&self) {
        self.call(self.table.0.device_pause);
    }
    fn resume(&self) {
        self.call(self.table.0.device_resume);
    }
}

struct ShellHost {
    table: Arc<Table>,
    scene: *const SceneHandle,
    apple: bool,
}
// SAFETY: The shell keeps the scene handle alive and unchanged for the session's lifetime.
unsafe impl Send for ShellHost {}
unsafe impl Sync for ShellHost {}

impl backend::Host for ShellHost {
    fn validate_screens(
        &self,
        screens: &mradm_dsp::room::screen::RoomScreens,
    ) -> Result<(), (i32, String)> {
        FfiHost.validate_screens(screens)
    }
    fn prepare_room(
        &self,
        package: &mradm_dsp::room::loading::Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        metres: f64,
    ) -> Result<mradm_render::room_runtime::Resources, (i32, String)> {
        FfiHost.prepare_room(package, hrir, sofa, quality, metres)
    }
    fn room_polar(&self) -> mradm_render::room::PolarConversion {
        FfiHost.room_polar()
    }
    fn room_scope(&self) -> mradm_render::room_runtime::Scope {
        FfiHost.room_scope()
    }
    fn room_switch(&self) -> mradm_render::room_runtime::Prepare {
        FfiHost.room_switch()
    }
    fn room_worker(&self, active: bool) {
        FfiHost.room_worker(active);
    }
    fn room_wait(&self, duration: std::time::Duration) {
        FfiHost.room_wait(duration);
    }
}

impl SessionHost for ShellHost {
    fn scene(&self) -> Option<&RenderScene> {
        unsafe { self.scene.as_ref() }.map(SceneHandle::scene)
    }
    fn apple(&self) -> bool {
        self.apple
    }
    fn device(&self, choice: &Token, device_id: &[u8]) -> Arc<dyn Device> {
        let device = self.table.0.device.map_or(ptr::null_mut(), |f| unsafe {
            f(
                self.table.0.ctx,
                options_ctx(choice),
                device_id.as_ptr(),
                device_id.len(),
            )
        });
        Arc::new(ShellDevice {
            table: self.table.clone(),
            device,
        })
    }
    fn open_apple(
        &self,
        choice: &Token,
        layout: &str,
        matrix: Option<&direct_matrix::Matrix>,
    ) -> Result<Box<dyn Source>, Error> {
        let Some(f) = self.table.0.open_apple else {
            return Err(bad("Apple backend is unavailable"));
        };
        let matrix = matrix.cloned().map(DirectMatrix::new);
        let view = matrix.as_ref().map(|m| m.view());
        let mut island = std::mem::MaybeUninit::<EngineIslandStream>::zeroed();
        let ctx = self.table.0.ctx;
        Buffers::new().call(|e| unsafe {
            f(
                ctx,
                options_ctx(choice),
                layout.as_ptr(),
                layout.len(),
                view.as_ref().map_or(ptr::null(), |v| v as *const _),
                island.as_mut_ptr(),
                e,
            )
        })?;
        Ok(Box::new(IslandSource::new(unsafe { island.assume_init() })))
    }
}

/// A shared HpTF profile (`HptfSettings`); design errors keep the former 512-byte buffer.
struct Design(Settings);

impl HptfDesign for Design {
    fn design(&self, sample_rate: u32) -> Result<mradm_dsp::hptf::StereoSnapshot, Error> {
        self.0.design(sample_rate).map_err(|(code, message)| {
            let mut len = message.len().min(511);
            while !message.is_char_boundary(len) {
                len -= 1;
            }
            Error {
                code,
                message: message.as_bytes()[..len].to_vec(),
                context: Vec::new(),
            }
        })
    }
}

/// One backend choice from a request record; the options are released on failure too.
unsafe fn choice(
    table: &Arc<Table>,
    request: *const EngineRenderRequest,
    system_spatial: u32,
    options: *mut c_void,
) -> Result<Choice, Error> {
    let token: Token = Arc::new(Options {
        table: table.clone(),
        ctx: options,
    });
    let request = unsafe { request.as_ref() }
        .and_then(|r| unsafe { request_of(r) })
        .ok_or_else(|| bad("invalid monitor request (UTF-8 paths without NUL required)"))?;
    Ok(Choice {
        request,
        system_spatial: system_spatial != 0,
        token,
    })
}

unsafe fn session<'a>(handle: *const EngineMonitorSession) -> Option<&'a EngineMonitorSession> {
    unsafe { handle.as_ref() }
}

/// A session without an engine; null for an invalid host or platform record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_new(
    platform_record: *const EnginePlatform,
    scene: *const SceneHandle,
    sample_rate: u32,
    total_frames: u64,
    host: *const EngineMonitorSessionHost,
) -> *mut EngineMonitorSession {
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(host), Some(apple)) = (
            unsafe { host.as_ref() },
            unsafe { platform_record.as_ref() }
                .and_then(|p| unsafe { platform(p) })
                .map(|p| p.apple.is_some()),
        ) else {
            return ptr::null_mut();
        };
        let table = Arc::new(Table(EngineMonitorSessionHost { ..*host }));
        let shell = Arc::new(ShellHost {
            table: table.clone(),
            scene,
            apple,
        });
        Box::into_raw(Box::new(EngineMonitorSession {
            session: Session::new(sample_rate, total_frames, shell),
            table,
            entry: Mutex::new(None),
        }))
    }))
    .unwrap_or(ptr::null_mut())
}

/// Appends a log line (level: `adm::LogLevel`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_log(
    handle: *const EngineMonitorSession,
    level: u32,
    module: *const u8,
    module_len: usize,
    text: *const u8,
    text_len: usize,
) {
    let Some(h) = (unsafe { session(handle) }) else {
        return;
    };
    let (Ok(module), Ok(text)) = (unsafe { input(module, module_len) }, unsafe {
        input(text, text_len)
    }) else {
        return;
    };
    h.session.log(level, &String::from_utf8_lossy(module), text);
}

/// Opens the first stream on a device for `device_id` and starts it. Code 100: the C++ session
/// threw (the shell rethrows the message). The options are taken over in every case.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_start(
    handle: *const EngineMonitorSession,
    request: *const EngineRenderRequest,
    system_spatial: u32,
    options: *mut c_void,
    device_id: *const u8,
    device_id_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        let choice = unsafe { choice(&h.table, request, system_spatial, options)? };
        h.session
            .start(choice, unsafe { input(device_id, device_id_len)? })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_destroy(handle: *mut EngineMonitorSession) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_play(handle: *const EngineMonitorSession) {
    if let Some(h) = unsafe { session(handle) } {
        h.session.play();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_pause(handle: *const EngineMonitorSession) {
    if let Some(h) = unsafe { session(handle) } {
        h.session.pause();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_seek(
    handle: *const EngineMonitorSession,
    seconds: f64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        h.session.seek_seconds(seconds)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_set_loop(
    handle: *const EngineMonitorSession,
    start: f64,
    end: f64,
) {
    if let Some(h) = unsafe { session(handle) } {
        h.session.set_loop_seconds(start, end);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_overrides(
    handle: *const EngineMonitorSession,
    values: *const RenderLiveOverride,
    values_len: usize,
    revision: u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        h.session
            .set_overrides(unsafe { overrides_of(values, values_len, revision)? })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_orientation(
    handle: *const EngineMonitorSession,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Some(h) = unsafe { session(handle) } {
        h.session.set_orientation([yaw, pitch, roll]);
    }
}

/// `switch_backend`; the options are taken over in every case.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_switch(
    handle: *const EngineMonitorSession,
    request: *const EngineRenderRequest,
    system_spatial: u32,
    options: *mut c_void,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        let choice = unsafe { choice(&h.table, request, system_spatial, options)? };
        h.session.switch_backend(choice)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_output_device(
    handle: *const EngineMonitorSession,
    device_id: *const u8,
    device_id_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        h.session
            .set_output_device(unsafe { input(device_id, device_id_len)? })
    })
}

/// `set_hptf_prepared` with a `mradm_dsp_hptf_settings_create` handle (copied).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_hptf(
    handle: *const EngineMonitorSession,
    settings: *const c_void,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        let settings = unsafe { settings.cast::<Settings>().as_ref() }
            .ok_or_else(|| bad("HpTF:配置指针无效"))?;
        h.session.set_hptf(Arc::new(Design(settings.clone())))
    })
}

/// The HpTF target the callback has taken up; 0 without an engine.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_hptf_snapshot(
    handle: *const EngineMonitorSession,
    out: *mut Snapshot,
) -> u32 {
    let (Some(h), Some(out)) = (unsafe { session(handle) }, unsafe { out.as_mut() }) else {
        return 0;
    };
    let Some(snapshot) = h.session.hptf_snapshot() else {
        return 0;
    };
    *out = snapshot.into();
    1
}

/// 0 for an invalidated session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_status(
    handle: *const EngineMonitorSession,
    out: *mut EngineLiveMonitorStatus,
) -> u32 {
    let (Some(h), Some(out)) = (unsafe { session(handle) }, unsafe { out.as_mut() }) else {
        return 0;
    };
    let Some(s) = h.session.status() else {
        return 0;
    };
    *out = EngineLiveMonitorStatus {
        state: s.state as u32,
        playhead_frames: s.playhead_frames,
        underruns: s.underruns,
        buffered_frames: s.buffered_frames,
        ring_fill: s.ring_fill,
        ended: u32::from(s.ended),
        failed: u32::from(s.failed),
        override_revision: s.override_revision,
    };
    1
}

/// 0 for an invalidated session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_levels(
    handle: *const EngineMonitorSession,
    out: *mut EngineLiveMonitorLevels,
) -> u32 {
    let (Some(h), Some(out)) = (unsafe { session(handle) }, unsafe { out.as_mut() }) else {
        return 0;
    };
    let Some(l) = h.session.levels() else {
        return 0;
    };
    *out = EngineLiveMonitorLevels {
        channels: l.channels,
        peak: l.peak,
        rms: l.rms,
        momentary_lufs: l.momentary_lufs,
        shortterm_lufs: l.shortterm_lufs,
        integrated_lufs: l.integrated_lufs,
    };
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_log_count(
    handle: *const EngineMonitorSession,
) -> usize {
    unsafe { session(handle) }.map_or(0, |h| h.session.log_count())
}

/// One buffered log line; the texts stay valid until the next call on this session.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_log_entry(
    handle: *const EngineMonitorSession,
    index: usize,
    level: *mut u32,
    module: *mut Text,
    text: *mut Text,
) -> u32 {
    let Some(h) = (unsafe { session(handle) }) else {
        return 0;
    };
    let Some(entry) = h.session.log_entry(index) else {
        return 0;
    };
    let mut slot = h.entry.lock().unwrap_or_else(|e| e.into_inner());
    let (l, m, t) = slot.insert(entry);
    unsafe {
        if let Some(level) = level.as_mut() {
            *level = *l;
        }
        if let Some(module) = module.as_mut() {
            *module = Text::new(m);
        }
        if let Some(text) = text.as_mut() {
            *text = Text::bytes(t);
        }
    }
    1
}

/// The current stream's room control, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_room_control(
    handle: *const EngineMonitorSession,
) -> *mut RenderRoomControl {
    unsafe { session(handle) }
        .and_then(|h| h.session.room_control())
        .map_or(ptr::null_mut(), control_handle)
}

/// Device callback of a session's engine: never waits or allocates.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_session_monitor_pull(
    monitor: *const EngineSessionMonitor,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    result: *mut EngineLiveMonitorPull,
) {
    let monitor = monitor.cast::<Monitor>();
    let (Some(m), Some(result)) = (unsafe { monitor.as_ref() }, unsafe { result.as_mut() }) else {
        return;
    };
    if output_len != 0 && (output.is_null() || output_len > isize::MAX as usize / 4) {
        std::process::abort();
    }
    let output: &mut [f32] = if output_len == 0 {
        &mut []
    } else {
        unsafe { std::slice::from_raw_parts_mut(output, output_len) }
    };
    let pulled = m.pull(output, frames);
    *result = EngineLiveMonitorPull::new(pulled.frames, pulled.mark_end);
}
