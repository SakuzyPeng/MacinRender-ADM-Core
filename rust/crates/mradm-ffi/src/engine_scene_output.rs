//! The Scene output session (`mradm-engine` `scene::output`) and its device: the session opens
//! the device (`device_island`), pulls it, and orders the device calls of play, pause, epochs,
//! volume and status as the C++ `SceneOutputSession` did. Errors use the render boundary.
use super::device_island::{
    Device, DeviceIslands, DeviceTable, Devices, HARDWARE, NULL_BACKEND, SYSTEM_SPATIAL,
};
use super::engine_scene::EngineSceneStream;
use super::{Error, bad, boundary};
use crate::adm::Text;
use crate::hptf::{Coefficients, Snapshot, settings::Settings};
use mradm_engine::scene::output::Session;
use mradm_scene::layouts::{self, Geometry};
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::{Mutex, MutexGuard};

/// A Scene output session handle; `session` drops before `device`.
pub struct EngineSceneOutput {
    session: Session,
    device: Device,
    /// Serialises the control calls (play, pause, epochs, volume, HpTF, status).
    control: Mutex<()>,
}

impl EngineSceneOutput {
    fn lock(&self) -> MutexGuard<'_, ()> {
        self.control.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn park(&self) {
        self.session.park();
        self.device.pause();
    }
}

impl Drop for EngineSceneOutput {
    fn drop(&mut self) {
        self.session.park();
        self.device.stop();
    }
}

/// `SceneOutputKind`.
const KIND_SYSTEM_SPATIAL: u32 = 0;
const KIND_STEREO: u32 = 1;
const KIND_NULL: u32 = 2;

/// `AudioDeviceProgress`.
#[repr(C)]
pub struct EngineSceneDeviceProgress {
    pub presented_frames: u64,
    pub media_clock: u32,
    pub failed: u32,
    pub recovering: u32,
    pub underruns: u64,
}

/// `SceneDeviceStatus`; `state` is `SceneOutputState`.
#[repr(C)]
pub struct EngineSceneOutputStatus {
    pub state: u32,
    pub epoch: u64,
    pub consumed_frames: u64,
    pub presented_frames: u64,
    pub queued_frames: u64,
    pub underruns: u64,
    pub media_clock: u32,
    pub recovering: u32,
    pub failed: u32,
    pub ended: u32,
}

unsafe fn output<'a>(handle: *const EngineSceneOutput) -> Result<&'a EngineSceneOutput, Error> {
    unsafe { handle.as_ref() }.ok_or_else(|| bad("无效 Scene 输出句柄"))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_destroy(handle: *mut EngineSceneOutput) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The device callback: pull the session, mark the device's end at EOS and release, as the
/// former C++ callback did. Never waits or allocates.
unsafe extern "C" fn pull(
    user: *mut c_void,
    output: *mut f32,
    output_len: usize,
    frames: usize,
) -> usize {
    let h = unsafe { &*user.cast::<EngineSceneOutput>() };
    let s = &h.session;
    if output_len != 0 && (output.is_null() || output_len > isize::MAX as usize / 4) {
        return 0;
    }
    let output: &mut [f32] = if output_len == 0 {
        &mut []
    } else {
        unsafe { std::slice::from_raw_parts_mut(output, output_len) }
    };
    // The pull counts itself before anything can fail, so a caught panic still releases.
    let (frames, eos, active) = match catch_unwind(AssertUnwindSafe(|| s.pull(output, frames))) {
        Ok(p) => (p.frames, p.eos, p.active),
        Err(_) => (0, false, true),
    };
    if active {
        if eos {
            h.device.mark_end();
        }
        s.release();
    }
    frames
}

/// The session over `device`, started and paused (`SceneOutputSession::create_with_device`).
fn start(
    stream: &EngineSceneStream,
    device: Device,
    protect_stereo: bool,
) -> Result<*mut EngineSceneOutput, Error> {
    let session = Session::new(stream.stream.clone(), protect_stereo, device.has_volume())?;
    let (channels, rate) = (session.channels(), session.sample_rate());
    let output = Box::new(EngineSceneOutput {
        session,
        device,
        control: Mutex::new(()),
    });
    let user = ptr::from_ref::<EngineSceneOutput>(&output)
        .cast_mut()
        .cast();
    // A failed start drops the output: park, then stop, as the C++ destructor did.
    output.device.start(channels, rate, user, pull)?;
    output.device.pause();
    Ok(Box::into_raw(output))
}

fn invalid(message: &str) -> Error {
    Error {
        code: 1,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

/// Opens the device of `kind` (`SceneOutputKind`: 0 system spatial over `layout` in `geometry`,
/// 1 the stereo hardware device `device_id`, 2 the null backend) and the session over it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_open(
    stream: *const EngineSceneStream,
    kind: u32,
    layout: Text,
    geometry: u32,
    device_id: Text,
    devices: *const DeviceIslands,
    out: *mut *mut EngineSceneOutput,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let out = unsafe { out.as_mut() }.ok_or_else(|| bad("无效输出指针"))?;
        *out = ptr::null_mut();
        let stream = unsafe { stream.as_ref() }.ok_or_else(|| bad("无效 Scene 流句柄"))?;
        let devices = Devices::new(devices).ok_or_else(|| bad("无效设备表"))?;
        let layout = String::from_utf8_lossy(unsafe { super::bytes(&layout)? }).into_owned();
        let device_id = unsafe { super::bytes(&device_id)? };
        let (_, channels) = stream.stream.output_format();
        let device = if kind == KIND_SYSTEM_SPATIAL {
            let geometry = if geometry == 1 {
                Geometry::Apple
            } else {
                Geometry::Standard
            };
            if layouts::find(&layout, geometry)
                .is_none_or(|l| l.speakers.len() != channels as usize)
            {
                return Err(invalid(
                    "Scene output layout does not match the rendered channels",
                ));
            }
            let geometry = u32::from(geometry == Geometry::Apple);
            devices.open(SYSTEM_SPATIAL, &layout, geometry, b"")?
        } else {
            if kind == KIND_STEREO && channels != 2 {
                return Err(invalid("Stereo output requires two rendered channels"));
            }
            let device_kind = if kind == KIND_NULL {
                NULL_BACKEND
            } else {
                HARDWARE
            };
            devices.open(device_kind, "", 0, device_id)?
        };
        *out = start(stream, device, kind != KIND_SYSTEM_SPATIAL)?;
        Ok(())
    })
}

/// The session over a caller's opened device (taken over in every case).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_create(
    stream: *const EngineSceneStream,
    device: *const DeviceTable,
    protect_stereo: u32,
    out: *mut *mut EngineSceneOutput,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    let device = unsafe { device.as_ref() }.map(|d| unsafe { Device::adopt(d) });
    boundary(message, message_len, context, context_len, || {
        let out = unsafe { out.as_mut() }.ok_or_else(|| bad("无效输出指针"))?;
        *out = ptr::null_mut();
        let stream = unsafe { stream.as_ref() }.ok_or_else(|| bad("无效 Scene 流句柄"))?;
        let device = device.ok_or_else(|| bad("无效设备"))?;
        *out = start(stream, device, protect_stereo != 0)?;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_pause(handle: *const EngineSceneOutput) {
    if let Ok(h) = unsafe { output(handle) } {
        let _control = h.lock();
        h.park();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_play(handle: *const EngineSceneOutput) {
    if let Ok(h) = unsafe { output(handle) } {
        let _control = h.lock();
        h.session.play();
        h.device.resume();
    }
}

/// A stale epoch is rejected before the device queue is disturbed; otherwise park, flush the
/// device and reset to `target`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_begin_epoch(
    handle: *const EngineSceneOutput,
    epoch: u64,
    target: i64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { output(handle)? };
        let _control = h.lock();
        h.session.check_epoch(epoch)?;
        h.park();
        h.device.flush();
        h.session.reset_epoch(epoch, target)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_set_volume(
    handle: *const EngineSceneOutput,
    gain: f32,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { output(handle)? };
        let _control = h.lock();
        h.session.set_volume(gain)?;
        h.device.set_volume(gain);
        Ok(())
    })
}

/// Publish designed coefficients, or the bypass when `coefficients` is null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_set_hptf(
    handle: *const EngineSceneOutput,
    coefficients: *const Coefficients,
    revision: u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { output(handle)? };
        // Only serialises concurrent setters; the publish itself is lock-free.
        let _control = h.lock();
        let coefficients = unsafe { coefficients.as_ref() }.map(|&c| c.into());
        h.session.set_hptf(coefficients, revision)
    })
}

/// Design a prepared profile for the stream's rate and publish it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_set_hptf_settings(
    handle: *const EngineSceneOutput,
    settings: *const Settings,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let s = &unsafe { output(handle)? }.session;
        s.check_hptf_settings()?;
        let settings = unsafe { settings.as_ref() }.ok_or_else(|| bad("HpTF:配置指针无效"))?;
        let snapshot = settings
            .design(s.sample_rate())
            .map_err(|(code, message)| Error {
                code,
                message: message.into_bytes(),
                context: Vec::new(),
            })?;
        s.publish_hptf(snapshot)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_hptf_snapshot(
    handle: *const EngineSceneOutput,
    out: *mut Snapshot,
) {
    if let (Ok(h), Some(out)) = (unsafe { output(handle) }, unsafe { out.as_mut() }) {
        *out = h.session.hptf_snapshot().into();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_status(
    handle: *const EngineSceneOutput,
    out: *mut EngineSceneOutputStatus,
) {
    let (Ok(h), Some(out)) = (unsafe { output(handle) }, unsafe { out.as_mut() }) else {
        return;
    };
    let _control = h.lock();
    let v = h.session.status(h.device.progress());
    *out = EngineSceneOutputStatus {
        state: v.state as u32,
        epoch: v.epoch,
        consumed_frames: v.consumed_frames,
        presented_frames: v.presented_frames,
        queued_frames: v.queued_frames,
        underruns: v.underruns,
        media_clock: u32::from(v.media_clock),
        recovering: u32::from(v.recovering),
        failed: u32::from(v.failed),
        ended: u32::from(v.ended),
    };
}
