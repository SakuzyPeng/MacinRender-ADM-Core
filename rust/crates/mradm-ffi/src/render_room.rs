//! Private room control, file stream and Scene stream boundary (PCM consistency 4.6 E). The room
//! runtime's session calls run through `crate::room::scope`, the boundary they used to cross.
use super::*;
use crate::room::{RoomProfileHandle, RoomSourceInfo, package, prepare_switch};
use mradm_dsp::room::quality::Quality;
use mradm_render::room_runtime::{Config, Control, Prepare};

/// `RoomConfig`: a profile handle, the UTF-8 personal HRIR path (empty for none), the scale and
/// the quality (0 exact, 1 realtime).
#[repr(C)]
pub struct RenderRoomConfig {
    profile: *const RoomProfileHandle,
    hrir: Text,
    metres: f64,
    quality: u32,
}

/// `RoomStatus`; `message` lives until the next status call on the control handle.
#[repr(C)]
pub struct RenderRoomStatus {
    state: u32,
    constrained_sources: u32,
    requested_revision: u64,
    applied_revision: u64,
    transition_sample: u64,
    material_frames: u64,
    tail_frames: u64,
    buffer_frames: u64,
    material_filter_guard_frames: u64,
    quality: u32,
    reflection_update_frames: u32,
    message: Text,
}

/// `IRoomControl`, independent of the stream or renderer that issued it.
pub struct RenderRoomControl {
    control: Control,
    message: Vec<u8>,
}

const SWITCH: &str = "房间切换必须保持当前档位、HRIR 和坐标比例";

unsafe fn room_config(r: *const RenderRoomConfig) -> Result<Config, Error> {
    let r = unsafe { r.as_ref() }.ok_or_else(|| bad("无效房间参数"))?;
    let Some(package) = (unsafe { package(r.profile) }) else {
        return Err(Error {
            code: 1,
            message: SWITCH.as_bytes().to_vec(),
            context: Vec::new(),
        });
    };
    Ok(Config {
        package,
        hrir: String::from_utf8_lossy(unsafe { bytes(&r.hrir)? }).into_owned(),
        metres: r.metres,
        quality: Quality::try_from(r.quality).map_err(|_| bad("无效房间档位"))?,
    })
}

pub(crate) fn switch() -> Prepare {
    Arc::new(prepare_switch)
}

pub(crate) fn control_handle(control: Control) -> *mut RenderRoomControl {
    Box::into_raw(Box::new(RenderRoomControl {
        control,
        message: Vec::new(),
    }))
}

unsafe fn room_control<'a>(handle: *const RenderRoomControl) -> Option<&'a Control> {
    if handle.is_null() {
        return None;
    }
    // Only borrow the shared control field: status may mutate the separate message buffer.
    Some(unsafe { &(*handle).control })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_control_request(
    handle: *const RenderRoomControl,
    config: *const RenderRoomConfig,
    revision: *mut u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let (Some(control), Some(out)) = (unsafe { room_control(handle) }, unsafe {
            revision.as_mut()
        }) else {
            return Err(bad("无效房间控制参数"));
        };
        *out = control.request(&unsafe { room_config(config)? })?;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_control_status(
    handle: *mut RenderRoomControl,
    out: *mut RenderRoomStatus,
) {
    let (Some(control), Some(out)) = (unsafe { room_control(handle) }, unsafe { out.as_mut() })
    else {
        return;
    };
    let s = control.status();
    // The caller serializes status calls and copying the returned message. Other queries
    // borrow only `control`, so they remain independent of this exclusive field borrow.
    let message = unsafe { &mut (*handle).message };
    *message = s.message.into_bytes();
    *out = RenderRoomStatus {
        state: s.state,
        constrained_sources: s.constrained_sources,
        requested_revision: s.requested_revision,
        applied_revision: s.applied_revision,
        transition_sample: s.transition_sample,
        material_frames: s.material_frames,
        tail_frames: s.tail_frames,
        buffer_frames: s.buffer_frames,
        material_filter_guard_frames: s.material_filter_guard_frames,
        quality: s.quality as u32,
        reflection_update_frames: s.reflection_update_frames,
        message: Text::bytes(message),
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_control_source(
    handle: *const RenderRoomControl,
    id: u64,
    out: *mut RoomSourceInfo,
) {
    let (Some(control), Some(out)) = (unsafe { room_control(handle) }, unsafe { out.as_mut() })
    else {
        return;
    };
    let p = control.source_position(id);
    *out = RoomSourceInfo {
        id: p.element_id,
        position: p.position,
        direction: p.direction,
        distance: p.distance,
        constrained: u32::from(p.constrained),
        present: u32::from(p.present),
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_control_destroy(handle: *mut RenderRoomControl) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The room control of a Scene stream room renderer; null for another renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_room_control(
    handle: *const RenderLive,
) -> *mut RenderRoomControl {
    match unsafe { handle.as_ref() }.and_then(|h| h.renderer.control()) {
        Some(control) => control_handle(control),
        _ => ptr::null_mut(),
    }
}

#[cfg(test)]
#[path = "render_room_control_test.rs"]
mod tests;
