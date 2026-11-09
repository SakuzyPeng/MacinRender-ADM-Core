//! The Scene output session (`mradm-engine` `scene::output`, PCM consistency 5.4 C) behind the C++
//! `SceneOutputSession` shell, which keeps the device. Errors use the render boundary.
use super::engine_scene::EngineSceneStream;
use super::{Error, bad, boundary};
use crate::hptf::{Coefficients, Snapshot, settings::Settings};
use mradm_engine::scene::output::{DeviceProgress, Session};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;

/// A Scene output session handle.
pub struct EngineSceneOutput {
    session: Session,
}

/// One device callback's result; while `active` the shell marks the device's end (when `eos`)
/// and then calls `mradm_engine_scene_output_release`.
#[repr(C)]
pub struct EngineSceneOutputPull {
    pub frames: usize,
    pub eos: u32,
    pub active: u32,
}

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

unsafe fn session<'a>(handle: *const EngineSceneOutput) -> Result<&'a Session, Error> {
    unsafe { handle.as_ref() }
        .map(|h| &h.session)
        .ok_or_else(|| bad("无效 Scene 输出句柄"))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_create(
    stream: *const EngineSceneStream,
    protect_stereo: u32,
    device_volume: u32,
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
        let session = Session::new(
            stream.stream.clone(),
            protect_stereo != 0,
            device_volume != 0,
        )?;
        *out = Box::into_raw(Box::new(EngineSceneOutput { session }));
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_destroy(handle: *mut EngineSceneOutput) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// Device callback. `output` holds `output_len` floats; never waits or allocates.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_pull(
    handle: *const EngineSceneOutput,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    result: *mut EngineSceneOutputPull,
) {
    let Some(result) = (unsafe { result.as_mut() }) else {
        return;
    };
    *result = EngineSceneOutputPull {
        frames: 0,
        eos: 0,
        active: 0,
    };
    let Ok(s) = (unsafe { session(handle) }) else {
        return;
    };
    if output_len != 0 && (output.is_null() || output_len > isize::MAX as usize / 4) {
        return;
    }
    let output: &mut [f32] = if output_len == 0 {
        &mut []
    } else {
        unsafe { std::slice::from_raw_parts_mut(output, output_len) }
    };
    // The pull counts itself before anything can fail, so a caught panic still releases.
    let pulled = catch_unwind(AssertUnwindSafe(|| s.pull(output, frames)));
    *result = match pulled {
        Ok(p) => EngineSceneOutputPull {
            frames: p.frames,
            eos: u32::from(p.eos),
            active: u32::from(p.active),
        },
        Err(_) => EngineSceneOutputPull {
            frames: 0,
            eos: 0,
            active: 1,
        },
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_release(handle: *const EngineSceneOutput) {
    if let Ok(s) = unsafe { session(handle) } {
        s.release();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_park(handle: *const EngineSceneOutput) {
    if let Ok(s) = unsafe { session(handle) } {
        s.park();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_play(handle: *const EngineSceneOutput) {
    if let Ok(s) = unsafe { session(handle) } {
        s.play();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_check_epoch(
    handle: *const EngineSceneOutput,
    epoch: u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        unsafe { session(handle)? }.check_epoch(epoch)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_reset_epoch(
    handle: *const EngineSceneOutput,
    epoch: u64,
    target: i64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        unsafe { session(handle)? }.reset_epoch(epoch, target)
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
        unsafe { session(handle)? }.set_volume(gain)
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
        let s = unsafe { session(handle)? };
        let coefficients = unsafe { coefficients.as_ref() }.map(|&c| c.into());
        s.set_hptf(coefficients, revision)
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
        let s = unsafe { session(handle)? };
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
    if let (Ok(s), Some(out)) = (unsafe { session(handle) }, unsafe { out.as_mut() }) {
        *out = s.hptf_snapshot().into();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_output_status(
    handle: *const EngineSceneOutput,
    device: *const EngineSceneDeviceProgress,
    out: *mut EngineSceneOutputStatus,
) {
    let (Ok(s), Some(device), Some(out)) = (
        unsafe { session(handle) },
        unsafe { device.as_ref() },
        unsafe { out.as_mut() },
    ) else {
        return;
    };
    let v = s.status(DeviceProgress {
        presented_frames: device.presented_frames,
        media_clock: device.media_clock != 0,
        failed: device.failed != 0,
        recovering: device.recovering != 0,
        underruns: device.underruns,
    });
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
