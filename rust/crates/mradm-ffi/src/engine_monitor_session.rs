//! The monitor session (`mradm-engine` `monitor::session`) behind the C++
//! `MonitorSession` shell. The shell registers the output devices and the Apple islands; the
//! session opens and drives the devices itself.
use super::apple_island::{Apple, AppleIslands, Settings as AppleSettings, log_context};
use super::device_island::{
    Device as OutputDevice, DeviceIslands, Devices, HARDWARE, SYSTEM_SPATIAL,
    output as device_output,
};
use super::engine_monitor::{EngineLiveMonitorLevels, EngineLiveMonitorStatus, overrides_of};
use super::room_ffi::{RenderRoomControl, control_handle};
use super::*;
use crate::engine::{Facts, PlatformIslands, platform};
use crate::engine_render::FfiHost;
use crate::engine_service::{EngineRenderRequest, empty_reply, request_of};
use crate::hptf::Snapshot;
use crate::hptf::settings::Settings;
use crate::scene::SceneHandle;
use mradm_engine::backend;
use mradm_engine::monitor::session::{
    AppleOpen, Choice, Device, Entry, HptfDesign, Session, SessionHost,
};
use mradm_engine::monitor::{Monitor, Source};
use mradm_engine::request::normalize_output_layout;
use mradm_scene::RenderScene;
use std::ffi::c_void;
use std::sync::{Arc, Mutex};

/// The shell's islands: the output devices and the Apple backend (either may be null).
#[repr(C)]
pub struct EngineMonitorSessionHost {
    pub devices: *const DeviceIslands,
    pub apple: *const AppleIslands,
}

pub struct EngineMonitorSession {
    session: Session,
    entry: Mutex<Option<Entry>>,
}

/// A session device: an opened device table, or the error opening it gave (reported by `start`).
struct IslandDevice {
    device: Result<IslandOutput, Error>,
}

/// The device and the monitor its callback pulls.
struct IslandOutput {
    device: OutputDevice,
    pull: Mutex<Option<Box<Puller>>>,
}

/// What a monitor device's callback reads: the monitor and the device (for `mark_end`).
struct Puller {
    monitor: *const Monitor,
    device: *const OutputDevice,
}
// SAFETY: Both outlive the device's callbacks (the session stops the device first) and are only read.
unsafe impl Send for Puller {}
unsafe impl Sync for Puller {}

/// The callback of a session's engine: never waits or allocates.
unsafe extern "C" fn pull_monitor(
    user: *mut c_void,
    out: *mut f32,
    len: usize,
    frames: usize,
) -> usize {
    let p = unsafe { &*user.cast::<Puller>() };
    let output = unsafe { device_output(out, len) };
    let pulled = unsafe { &*p.monitor }.pull(output, frames);
    if pulled.mark_end {
        unsafe { &*p.device }.mark_end();
    }
    pulled.frames
}

impl Device for IslandDevice {
    fn realtime(&self) -> bool {
        self.device.as_ref().is_ok_and(|d| d.device.realtime())
    }
    fn start(&self, channels: usize, sample_rate: u32, monitor: &Monitor) -> Result<(), Error> {
        let d = self.device.as_ref().map_err(Clone::clone)?;
        // SAFETY of the pointers: the session stops this device before the monitor drops, and the
        // puller lives with the device.
        let mut slot = d.pull.lock().unwrap_or_else(|e| e.into_inner());
        let puller = slot.insert(Box::new(Puller {
            monitor,
            device: &d.device,
        }));
        let user = ptr::from_mut::<Puller>(puller).cast();
        d.device
            .start(channels as u32, sample_rate, user, pull_monitor)
    }
    fn stop(&self) {
        if let Ok(d) = &self.device {
            d.device.stop();
        }
    }
    fn flush(&self) {
        if let Ok(d) = &self.device {
            d.device.flush();
        }
    }
    fn pause(&self) {
        if let Ok(d) = &self.device {
            d.device.pause();
        }
    }
    fn resume(&self) {
        if let Ok(d) = &self.device {
            d.device.resume();
        }
    }
}

struct ShellHost {
    devices: Option<Devices>,
    apple_islands: *const AppleIslands,
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
    fn device(&self, choice: &Choice, device_id: &[u8]) -> Arc<dyn Device> {
        let Some(devices) = self.devices else {
            return Arc::new(IslandDevice {
                device: Err(bad("output devices are unavailable")),
            });
        };
        let r = &choice.request;
        let (kind, layout, geometry, id) = device_choice(
            choice.system_spatial && devices.system_spatial(),
            &r.output_layout,
            r.apple_geometry,
            device_id,
        );
        let device = devices.open(kind, &layout, geometry, id);
        Arc::new(IslandDevice {
            device: device.map(|device| IslandOutput {
                device,
                pull: Mutex::new(None),
            }),
        })
    }
    fn open_apple(
        &self,
        plan: &AppleOpen<'_>,
        log: &dyn Fn(u32, &str, &[u8]),
    ) -> Result<Box<dyn Source>, Error> {
        let Some(apple) = Apple::new(self.apple_islands) else {
            return Err(bad("Apple backend is unavailable"));
        };
        let settings = AppleSettings::new(plan.input_path, plan.layout, plan.matrix, &plan.options);
        let context = log_context(&log);
        let mut reply = empty_reply();
        let Some(prepared) = apple.prepare(&settings, self.scene(), &context, &mut reply) else {
            let text = |t: &Text| unsafe { bytes(t) }.unwrap_or_default().to_vec();
            return Err(Error {
                code: reply.code,
                message: text(&reply.message),
                context: text(&reply.context),
            });
        };
        // The stream keeps the preparation.
        apple.open_stream(&prepared, &settings, &context)
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

/// The device of a choice: the system-spatial sink (where the platform has one) follows the system
/// media route over the normalised layout, keeping a device refresh on the same sink; otherwise the
/// hardware device `device_id`.
fn device_choice<'a>(
    system_spatial: bool,
    output_layout: &str,
    apple_geometry: bool,
    device_id: &'a [u8],
) -> (u32, String, u32, &'a [u8]) {
    if system_spatial {
        (
            SYSTEM_SPATIAL,
            normalize_output_layout(output_layout),
            u32::from(apple_geometry),
            b"",
        )
    } else {
        (HARDWARE, String::new(), 0, device_id)
    }
}

/// One backend choice from a request record.
unsafe fn choice(
    request: *const EngineRenderRequest,
    system_spatial: u32,
) -> Result<Choice, Error> {
    let request = unsafe { request.as_ref() }
        .and_then(|r| unsafe { request_of(r) })
        .ok_or_else(|| bad("invalid monitor request (UTF-8 paths without NUL required)"))?;
    Ok(Choice {
        request,
        system_spatial: system_spatial != 0,
        token: Arc::new(()),
    })
}

unsafe fn session<'a>(handle: *const EngineMonitorSession) -> Option<&'a EngineMonitorSession> {
    unsafe { handle.as_ref() }
}

/// A session without an engine; null for an invalid host or platform record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_new(
    islands: *const PlatformIslands,
    scene: *const SceneHandle,
    sample_rate: u32,
    total_frames: u64,
    host: *const EngineMonitorSessionHost,
) -> *mut EngineMonitorSession {
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(host), Some(apple)) = (
            unsafe { host.as_ref() },
            unsafe { islands.as_ref() }
                .and_then(|p| unsafe { platform(p, Facts::Render) })
                .map(|p| p.apple.is_some()),
        ) else {
            return ptr::null_mut();
        };
        let shell = Arc::new(ShellHost {
            devices: Devices::new(host.devices),
            apple_islands: host.apple,
            scene,
            apple,
        });
        Box::into_raw(Box::new(EngineMonitorSession {
            session: Session::new(sample_rate, total_frames, shell),
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

/// Opens the first stream on a device for `device_id` and starts it. Code 100: the former C++
/// session threw (the shell rethrows the message).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_start(
    handle: *const EngineMonitorSession,
    request: *const EngineRenderRequest,
    system_spatial: u32,
    device_id: *const u8,
    device_id_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        let choice = unsafe { choice(request, system_spatial)? };
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

/// `switch_backend`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_monitor_session_switch(
    handle: *const EngineMonitorSession,
    request: *const EngineRenderRequest,
    system_spatial: u32,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { session(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        let choice = unsafe { choice(request, system_spatial)? };
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::device_island::DeviceTable;
    use std::sync::Mutex as StdMutex;

    static OPENED: StdMutex<Vec<String>> = StdMutex::new(Vec::new());

    unsafe extern "C" fn open(
        kind: u32,
        layout: Text,
        geometry: u32,
        id: Text,
        _: *mut DeviceTable,
        e: *const crate::render::engine_monitor::EngineIslandError,
    ) -> i32 {
        let text = |t: &Text| String::from_utf8(unsafe { bytes(t) }.unwrap().to_vec()).unwrap();
        OPENED
            .lock()
            .unwrap()
            .push(format!("{kind} {} {geometry} {}", text(&layout), text(&id)));
        if kind == HARDWARE && text(&id) == "missing" {
            return unsafe {
                crate::render::engine_monitor::island_error_for_test(e, 2, "no device")
            };
        }
        0
    }

    #[test]
    fn devices_follow_the_choice_and_report_open_errors() {
        assert_eq!(
            device_choice(true, "7.1.4", true, b"usb"),
            (
                SYSTEM_SPATIAL,
                normalize_output_layout("7.1.4"),
                1,
                &b""[..]
            )
        );
        assert_eq!(
            device_choice(false, "7.1.4", true, b"usb"),
            (HARDWARE, String::new(), 0, &b"usb"[..])
        );
        let table = DeviceIslands::for_test(1, Some(open));
        let devices = Devices::new(&table).unwrap();
        assert!(devices.system_spatial());
        assert!(devices.open(SYSTEM_SPATIAL, "4+7+0", 1, b"").is_ok());
        let failed = devices.open(HARDWARE, "", 0, b"missing").err().unwrap();
        assert_eq!((failed.code, &failed.message[..]), (2, &b"no device"[..]));
        assert_eq!(*OPENED.lock().unwrap(), ["0 4+7+0 1 ", "1  0 missing"]);
    }
}
