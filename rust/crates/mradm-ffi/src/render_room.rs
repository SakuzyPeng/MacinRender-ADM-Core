//! Private room control, file stream and Scene stream boundary (PCM consistency 4.6 E). The room
//! runtime's session calls run through `crate::room::scope`, the boundary they used to cross.
use super::*;
use crate::room::{
    RoomPreparedHandle, RoomProfileHandle, RoomSourceInfo, package, prepare_switch, resources,
    scope,
};
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::RoomScreens;
use mradm_render::room_runtime::{Config, Control, Prepare};
use mradm_render::room_stream::{FileStream, Live};

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

fn switch() -> Prepare {
    Arc::new(prepare_switch)
}

fn control_handle(control: Control) -> *mut RenderRoomControl {
    Box::into_raw(Box::new(RenderRoomControl {
        control,
        message: Vec::new(),
    }))
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
        let (Some(h), Some(out)) = (unsafe { handle.as_ref() }, unsafe { revision.as_mut() })
        else {
            return Err(bad("无效房间控制参数"));
        };
        *out = h.control.request(&unsafe { room_config(config)? })?;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_control_status(
    handle: *mut RenderRoomControl,
    out: *mut RenderRoomStatus,
) {
    let (Some(h), Some(out)) = (unsafe { handle.as_mut() }, unsafe { out.as_mut() }) else {
        return;
    };
    let s = h.control.status();
    h.message = s.message.into_bytes();
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
        message: Text::bytes(&h.message),
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_control_source(
    handle: *const RenderRoomControl,
    id: u64,
    out: *mut RoomSourceInfo,
) {
    let (Some(h), Some(out)) = (unsafe { handle.as_ref() }, unsafe { out.as_mut() }) else {
        return;
    };
    let p = h.control.source_position(id);
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

/// The room file stream (`IRenderStream`). Serialize calls; destroy after all calls return.
pub struct RenderRoomStream(FileStream);

unsafe fn room_stream<'a>(handle: *mut RenderRoomStream) -> offline::Result<&'a mut FileStream> {
    unsafe { handle.as_mut() }
        .map(|h| &mut h.0)
        .ok_or_else(|| driver_error("无效流式渲染句柄"))
}

/// Opens the stream of a room preparation (`prepared`) and its room set (`room`), seeked to 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_open(
    prepared: *const RenderPrepared,
    room: *const RoomPreparedHandle,
    path: *const u8,
    path_len: usize,
    config: *const RenderRoomConfig,
    pose: *const f32,
    output: *mut *mut RenderRoomStream,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let Some(out) = (unsafe { output.as_mut() }) else {
            return Err(driver_error("无效流式渲染参数"));
        };
        *out = ptr::null_mut();
        let (Some(table), Some(resources)) = (
            unsafe { prepared.as_ref() }.and_then(|p| p.room.clone()),
            unsafe { resources(room) },
        ) else {
            return Err(driver_error("无效流式渲染参数"));
        };
        let pose = unsafe { input(pose, 3) }
            .map_err(|e| driver_error(&String::from_utf8_lossy(&e.message)))?;
        let config = unsafe { room_config(config) }
            .map_err(|e| driver_error(&String::from_utf8_lossy(&e.message)))?;
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let stream = FileStream::open(
            path,
            table,
            resources,
            config,
            [pose[0], pose[1], pose[2]],
            scope,
            switch(),
        )?;
        *out = Box::into_raw(Box::new(RenderRoomStream(stream)));
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_process(
    handle: *mut RenderRoomStream,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    produced: *mut usize,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { produced.as_mut() }.ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = 0;
        let s = unsafe { room_stream(handle)? };
        let buffer = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = s.process(buffer, frames)?;
        Ok(())
    })
}

/// `*pending` (set unless the call fails) is whether a history block remains to be stepped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_seek_begin(
    handle: *mut RenderRoomStream,
    frame: u64,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { room_stream(handle)? }.seek_begin(frame)?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_seek_step(
    handle: *mut RenderRoomStream,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { room_stream(handle)? }.seek_step()?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_validate(
    handle: *mut RenderRoomStream,
    values: *const RenderTripleBalanceOverride,
    values_len: usize,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    boundary(
        message,
        message_len,
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let s = unsafe { room_stream(handle) }.map_err(|e| bad(&e.message))?;
            let o = unsafe { tb_overrides(values, values_len) }.map_err(|e| bad(&e.message))?;
            s.validate_overrides(&o).map_err(|(code, message)| Error {
                code,
                message: message.into_bytes(),
                context: Vec::new(),
            })
        },
    )
}

/// Latches a rejected update for the next `process`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_overrides(
    handle: *mut RenderRoomStream,
    values: *const RenderTripleBalanceOverride,
    values_len: usize,
) -> i32 {
    driver_boundary(ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), || {
        let o = unsafe { tb_overrides(values, values_len)? };
        unsafe { room_stream(handle)? }.set_overrides(&o);
        Ok(())
    })
}

/// Latches a rejected orientation for the next `process`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_pose(
    handle: *mut RenderRoomStream,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Some(h) = unsafe { handle.as_mut() } {
        h.0.set_pose([yaw, pitch, roll]);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_tail(handle: *const RenderRoomStream) -> u64 {
    unsafe { handle.as_ref() }.map_or(0, |h| h.0.tail_frames())
}

/// A new control handle; null for a null stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_control(
    handle: *const RenderRoomStream,
) -> *mut RenderRoomControl {
    unsafe { handle.as_ref() }.map_or(ptr::null_mut(), |h| control_handle(h.0.control()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_room_stream_destroy(handle: *mut RenderRoomStream) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The Scene stream room renderer: `spread` is `binaural_spread_mode != none`; `sink` whether the
/// caller publishes diagnostics.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_room_create(
    room: *const RoomPreparedHandle,
    config: *const RenderRoomConfig,
    spread: u32,
    screens: *const RoomScreens,
    sink: u32,
    output: *mut *mut RenderLive,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let (Some(out), Some(resources), Some(screens)) = (
            unsafe { output.as_mut() },
            unsafe { resources(room) },
            unsafe { screens.as_ref() },
        ) else {
            return Err(bad("无效房间参数"));
        };
        *out = ptr::null_mut();
        let live = Live::new(
            resources,
            unsafe { room_config(config)? },
            spread != 0,
            *screens,
            sink != 0,
            scope,
            switch(),
        )?;
        *out = Box::into_raw(Box::new(RenderLive::new(LiveRenderer::Room(Box::new(
            live,
        )))));
        Ok(())
    })
}

/// The room control of a Scene stream room renderer; null for another renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_room_control(
    handle: *const RenderLive,
) -> *mut RenderRoomControl {
    match unsafe { handle.as_ref() }.map(|h| &h.renderer) {
        Some(LiveRenderer::Room(live)) => control_handle(live.control()),
        _ => ptr::null_mut(),
    }
}
