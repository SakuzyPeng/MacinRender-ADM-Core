//! The monitor session (`mradm-engine` `monitor::session`) behind the C++
//! `MonitorSession` shell. The shell registers the output devices and the Apple islands; the
//! session opens and drives the devices itself.
use super::apple_island::{Apple, AppleIslands, Settings as AppleSettings, log_context};
use super::device_island::{
    Device as OutputDevice, DeviceIslands, Devices, HARDWARE, SYSTEM_SPATIAL,
    output as device_output,
};
use super::*;
use crate::engine::{Facts, PlatformIslands, platform};
use crate::engine_render::FfiHost;
use crate::engine_service::empty_reply;
use crate::hptf::settings::Settings;
use crate::scene::SceneHandle;
use mradm_engine::backend;
use mradm_engine::monitor::session::{AppleOpen, Choice, Device, HptfDesign, Session, SessionHost};
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

/// The monitor session behind `adm_monitor_t`.
///
/// Unlike [`EngineSceneOutput`](super::engine_scene_output::EngineSceneOutput) it is not pinned:
/// a device callback reads a `Puller` that points into the session's `Box<Monitor>` and into the
/// device's own allocation (`Arc<dyn Device>`), never into this struct, and the session stops the
/// device before it replaces or frees the monitor. Moving the session moves only those pointers.
/// Keep the monitor and the devices in their own allocations if the engine session changes.
pub struct EngineMonitorSession {
    session: Session,
}

impl EngineMonitorSession {
    /// A session without an engine over `scene` (`mradm_engine_monitor_session_new`); `None` for
    /// a null or invalid platform table. Start it with [`Session::start`] on [`Self::session`].
    ///
    /// # Safety
    ///
    /// `islands` must be null or point to a valid platform island table (read only here). The
    /// host's device and Apple tables (each may be null) must point to immutable, thread-safe
    /// tables, and `scene` must be null or point to a scene handle that is not changed; all of them
    /// must remain valid until the session is dropped.
    ///
    /// ```compile_fail,E0133
    /// use mradm_ffi::api::monitor::{
    ///     EngineMonitorSession, EngineMonitorSessionHost,
    /// };
    ///
    /// fn create(host: &EngineMonitorSessionHost) {
    ///     EngineMonitorSession::new(std::ptr::null(), std::ptr::null(), 48_000, 0, host);
    /// }
    /// ```
    pub unsafe fn new(
        islands: *const PlatformIslands,
        scene: *const SceneHandle,
        sample_rate: u32,
        total_frames: u64,
        host: &EngineMonitorSessionHost,
    ) -> Option<Self> {
        // SAFETY: the caller's contract for `islands`.
        let apple = unsafe { islands.as_ref() }
            .and_then(|p| unsafe { platform(p, Facts::Render) })
            .map(|p| p.apple.is_some())?;
        let shell = Arc::new(ShellHost {
            devices: Devices::new(host.devices),
            apple_islands: host.apple,
            scene,
            apple,
        });
        Some(Self {
            session: Session::new(sample_rate, total_frames, shell),
        })
    }

    /// The engine session.
    pub fn session(&self) -> &Session {
        &self.session
    }
}

/// One backend choice of a session: the request and whether it asks for the system-spatial sink.
pub fn choice(request: mradm_engine::render::Request, system_spatial: bool) -> Choice {
    Choice {
        request,
        system_spatial,
        token: Arc::new(()),
    }
}

/// The HpTF profile of [`Session::set_hptf`] (copied); design errors keep the former 512-byte
/// message buffer.
pub fn hptf_design(settings: &Settings) -> Arc<dyn HptfDesign> {
    Arc::new(Design(settings.clone()))
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
