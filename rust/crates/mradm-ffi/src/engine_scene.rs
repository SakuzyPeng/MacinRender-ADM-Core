//! The Scene stream engine (`mradm-engine` `scene::stream`) behind the C++
//! `SceneStreamEngine` shell. Errors use the render boundary (code, message, context); the frame
//! records reuse the Scene renderer's entry and update records.
use super::room_ffi::{RenderRoomControl, control_handle};
use super::{
    EngineSceneRendererRequest, Error, RenderLiveEntry, RenderLiveUpdate, Text, bad, boundary,
    bytes, input, live_state, scene_renderer_config,
};
use crate::engine_render::FfiHost;
use mradm_engine::scene::stream::{
    self, Config, Descriptor, Entity, FrameInput, Identity, PlaneInput, PlaneInputs, Sizes, Stream,
};
use mradm_render::live;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::Arc;

/// `SceneStreamConfig` without the renderer; `manual_clock` selects the replay clock starting at
/// `clock_ns`; the `*_bytes` are the C++ `sizeof` the input budget counts.
#[repr(C)]
pub struct EngineSceneConfig {
    pub output_sample_rate: u32,
    pub input_queue_samples: u32,
    pub input_queue_bytes: u64,
    pub output_ring_frames: u32,
    pub startup_watermark_frames: u32,
    pub manual_clock: u32,
    pub clock_ns: i64,
    pub frame_bytes: usize,
    pub plane_bytes: usize,
    pub entry_bytes: usize,
    pub update_bytes: usize,
}

#[repr(C)]
pub struct EngineSceneEntity {
    pub id: Text,
    pub name: Text,
}

/// `live_scene::SemanticIdentity`; `present` is 0 for none.
#[repr(C)]
pub struct EngineSceneIdentity {
    pub present: u32,
    pub object_id: Text,
    pub object_name: Text,
    pub track_uid: Text,
    pub has_importance: u32,
    pub importance: i32,
    pub has_dialogue_id: u32,
    pub dialogue_id: i32,
    pub contents: *const EngineSceneEntity,
    pub contents_len: usize,
    pub programmes: *const EngineSceneEntity,
    pub programmes_len: usize,
}

/// `live_scene::ElementDescriptor`; `role` is the C++ enum value.
#[repr(C)]
pub struct EngineSceneElement {
    pub element_id: u64,
    pub role: u32,
    pub speaker_label: Text,
    pub has_position: u32,
    pub position: [f32; 3],
    pub flags: u64,
    pub identity: EngineSceneIdentity,
}

/// `ScenePcmPlaneView`: `samples[k * stride]` is sample `k`.
#[repr(C)]
pub struct EngineScenePlane {
    pub element_id: u64,
    pub samples: *const f32,
    pub sample_count: u32,
    pub stride: u32,
    pub has_signal: u32,
}

/// `SceneFrameView`.
#[repr(C)]
pub struct EngineSceneFrame {
    pub epoch: u64,
    pub generation: u64,
    pub start: i64,
    pub duration: u32,
    pub flags: u32,
    pub planes: *const EngineScenePlane,
    pub planes_len: usize,
    pub initial: *const RenderLiveEntry,
    pub initial_len: usize,
    pub updates: *const RenderLiveUpdate,
    pub updates_len: usize,
}

/// `ScenePullResult`.
#[repr(C)]
pub struct EngineScenePull {
    pub flags: u32,
    pub epoch: u64,
    pub first_media_frame: u64,
    pub media_frames: u32,
    pub requested_frames: u32,
}

/// `SceneStreamStatus`; `state` is the `SceneStreamState` value.
#[repr(C)]
pub struct EngineSceneStatus {
    pub state: u32,
    pub epoch: u64,
    pub generation: u64,
    pub queued_input_samples: u64,
    pub queued_input_bytes: u64,
    pub buffered_output_frames: u64,
    pub media_frames_pulled: u64,
    pub underruns: u64,
    pub semantic_degradations: u64,
    pub semantic_policy_revision: u64,
    pub ring_fill: f32,
    pub ended: u32,
    pub failed: u32,
    pub production_complete: u32,
}

/// `live_scene::Diagnostic` without its text; `message_len` is the text's full length.
#[repr(C)]
pub struct EngineSceneDiagnostic {
    pub level: u32,
    pub code: u32,
    pub epoch: u64,
    pub generation: u64,
    pub element: u64,
    pub field: u64,
    pub message_len: usize,
}

/// A Scene stream handle.
pub struct EngineSceneStream {
    pub(crate) stream: Arc<Stream>,
}

impl EngineSceneStream {
    /// The handle of a created stream (`mradm_engine_scene_create`).
    pub fn new(stream: Stream) -> Box<Self> {
        Box::new(Self {
            stream: Arc::new(stream),
        })
    }

    /// The stream, for Rust callers.
    pub fn stream(&self) -> &Arc<Stream> {
        &self.stream
    }
}

/// The checked Scene stream configuration and renderer, for Rust callers: a stream on the FFI
/// host (room floating-point environment, Windows worker scheduling).
pub fn create_stream(
    config: &Config,
    renderer: &mradm_engine::scene::renderer::Config<'_>,
) -> Result<Box<EngineSceneStream>, Error> {
    Ok(EngineSceneStream::new(Stream::create(
        config,
        renderer,
        Arc::new(FfiHost),
    )?))
}

unsafe fn stream<'a>(handle: *const EngineSceneStream) -> Result<&'a Stream, Error> {
    unsafe { handle.as_ref() }
        .map(|h| &*h.stream)
        .ok_or_else(|| bad("无效 Scene 流句柄"))
}

unsafe fn entities(data: *const EngineSceneEntity, len: usize) -> Result<Vec<Entity>, Error> {
    unsafe { input(data, len)? }
        .iter()
        .map(|e| {
            Ok(Entity {
                id: unsafe { bytes(&e.id)? }.to_vec(),
                name: unsafe { bytes(&e.name)? }.to_vec(),
            })
        })
        .collect()
}

unsafe fn descriptor(e: &EngineSceneElement) -> Result<Descriptor, Error> {
    let i = &e.identity;
    let identity = if i.present != 0 {
        Some(Identity {
            object_id: unsafe { bytes(&i.object_id)? }.to_vec(),
            object_name: unsafe { bytes(&i.object_name)? }.to_vec(),
            track_uid: unsafe { bytes(&i.track_uid)? }.to_vec(),
            importance: (i.has_importance != 0).then_some(i.importance),
            dialogue_id: (i.has_dialogue_id != 0).then_some(i.dialogue_id),
            contents: unsafe { entities(i.contents, i.contents_len)? },
            programmes: unsafe { entities(i.programmes, i.programmes_len)? },
        })
    } else {
        None
    };
    Ok(Descriptor {
        element_id: e.element_id,
        role: e.role,
        speaker_label: unsafe { bytes(&e.speaker_label)? }.to_vec(),
        has_position: e.has_position != 0,
        position: e.position,
        flags: e.flags,
        identity,
    })
}

/// The producer's planes, read in place.
struct RawPlanes<'a>(&'a [EngineScenePlane]);

impl PlaneInputs for RawPlanes<'_> {
    fn len(&self) -> usize {
        self.0.len()
    }
    fn plane(&self, plane: usize) -> PlaneInput {
        let p = &self.0[plane];
        PlaneInput {
            element_id: p.element_id,
            sample_count: p.sample_count,
            stride: p.stride,
            has_signal: p.has_signal != 0,
            has_samples: !p.samples.is_null(),
        }
    }
    fn sample(&self, plane: usize, index: usize) -> f32 {
        let p = &self.0[plane];
        // The engine reads a sample only after the plane passed its checks: signalled, non-null,
        // a non-zero stride whose span fits the address space, and `index` below the frame
        // duration. The caller's view holds those samples (the C ABI contract).
        unsafe { p.samples.add(index * p.stride as usize).read() }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_create(
    config: *const EngineSceneConfig,
    renderer: *const EngineSceneRendererRequest,
    output: *mut *mut EngineSceneStream,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let (Some(c), Some(r), Some(out)) = (
            unsafe { config.as_ref() },
            unsafe { renderer.as_ref() },
            unsafe { output.as_mut() },
        ) else {
            return Err(bad("无效 Scene 流参数"));
        };
        *out = ptr::null_mut();
        let renderer = unsafe { scene_renderer_config(r)? };
        let config = Config {
            output_sample_rate: c.output_sample_rate,
            input_queue_samples: c.input_queue_samples,
            input_queue_bytes: c.input_queue_bytes,
            output_ring_frames: c.output_ring_frames,
            startup_watermark_frames: c.startup_watermark_frames,
            manual_clock: (c.manual_clock != 0).then_some(c.clock_ns),
            sizes: Sizes {
                frame: c.frame_bytes,
                plane: c.plane_bytes,
                entry: c.entry_bytes,
                update: c.update_bytes,
            },
        };
        *out = Box::into_raw(create_stream(&config, &renderer)?);
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_destroy(handle: *mut EngineSceneStream) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The pulled PCM's rate and channels and the input rate; zeros for a null handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_format(
    handle: *const EngineSceneStream,
    output_rate: *mut u32,
    channels: *mut u32,
    input_rate: *mut u32,
) {
    let (rate, count, input) = unsafe { stream(handle) }.map_or((0, 0, 0), |s| {
        let (rate, count) = s.output_format();
        (rate, count, s.input_sample_rate())
    });
    unsafe {
        for (p, v) in [(output_rate, rate), (channels, count), (input_rate, input)] {
            if let Some(p) = p.as_mut() {
                *p = v;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_begin_epoch(
    handle: *const EngineSceneStream,
    epoch: u64,
    target: i64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        unsafe { stream(handle)? }.begin_epoch(epoch, target)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_configure(
    handle: *const EngineSceneStream,
    epoch: u64,
    generation: u64,
    elements: *const EngineSceneElement,
    elements_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let s = unsafe { stream(handle)? };
        let elements = unsafe { input(elements, elements_len)? }
            .iter()
            .map(|e| unsafe { descriptor(e) })
            .collect::<Result<Vec<_>, Error>>()?;
        s.configure_generation(epoch, generation, &elements)
    })
}

/// `*status` is the `SceneSubmitStatus` value when the call succeeds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_submit(
    handle: *const EngineSceneStream,
    frame: *const EngineSceneFrame,
    timeout_ms: i64,
    status: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let s = unsafe { stream(handle)? };
        let (Some(f), Some(status)) = (unsafe { frame.as_ref() }, unsafe { status.as_mut() })
        else {
            return Err(bad("无效 Scene 帧参数"));
        };
        let planes = RawPlanes(unsafe { input(f.planes, f.planes_len)? });
        let initial: Vec<live::Entry> = unsafe { input(f.initial, f.initial_len)? }
            .iter()
            .map(|e| live::Entry {
                element_id: e.element_id,
                state: live_state(&e.state),
            })
            .collect();
        let updates: Vec<live::Update> = unsafe { input(f.updates, f.updates_len)? }
            .iter()
            .map(|u| live::Update {
                element_id: u.element_id,
                offset: u.offset,
                ramp: u.ramp,
                jump: u.jump != 0,
                changed: u.changed,
                cleared: u.cleared,
                state: live_state(&u.state),
            })
            .collect();
        let view = FrameInput {
            epoch: f.epoch,
            generation: f.generation,
            start: f.start,
            duration: f.duration,
            flags: f.flags,
            planes: &planes,
            initial: &initial,
            updates: &updates,
        };
        *status = s.submit_frame(&view, timeout_ms)? as u32;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_signal_end(
    handle: *const EngineSceneStream,
    epoch: u64,
    end: i64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        unsafe { stream(handle)? }.signal_end(epoch, end)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_switch_backend(
    handle: *const EngineSceneStream,
    renderer: *const EngineSceneRendererRequest,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let s = unsafe { stream(handle)? };
        let r = unsafe { renderer.as_ref() }.ok_or_else(|| bad("无效 Scene 流式渲染参数"))?;
        s.switch_backend(&unsafe { scene_renderer_config(r)? })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_wait_idle(
    handle: *const EngineSceneStream,
    timeout_ms: i64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        unsafe { stream(handle)? }.wait_idle(timeout_ms)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_orientation(
    handle: *const EngineSceneStream,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Ok(s) = unsafe { stream(handle) } {
        s.set_listener_orientation([yaw, pitch, roll]);
    }
}

/// The current backend's room control, or null without a room renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_room_control(
    handle: *const EngineSceneStream,
) -> *mut RenderRoomControl {
    unsafe { stream(handle) }
        .ok()
        .and_then(Stream::room_control)
        .map_or(ptr::null_mut(), control_handle)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_policy(
    handle: *const EngineSceneStream,
    json: *const u8,
    json_len: usize,
    revision: u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let s = unsafe { stream(handle)? };
        s.set_semantic_policy_json(unsafe { input(json, json_len)? }, revision)
    })
}

/// The audio thread's pull of `frames` frames into `output` (`output_len` samples); does not
/// block or allocate. A rejected call zeroes nothing and reports `failed`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_pull(
    handle: *const EngineSceneStream,
    output: *mut f32,
    output_len: usize,
    frames: u32,
    result: *mut EngineScenePull,
) {
    let Some(result) = (unsafe { result.as_mut() }) else {
        return;
    };
    let pulled = catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { stream(handle) }.ok()?;
        let needed = frames as usize * s.output_format().1 as usize;
        if needed > output_len || (needed != 0 && output.is_null()) {
            return None;
        }
        let output = if needed == 0 {
            &mut [][..]
        } else {
            // Checked above: `output` holds `output_len >= needed` samples.
            unsafe { std::slice::from_raw_parts_mut(output, needed) }
        };
        Some(s.pull(output, frames))
    }))
    .ok()
    .flatten();
    let p = pulled.unwrap_or(stream::Pull {
        flags: stream::pull_flag::FAILED,
        requested_frames: frames,
        ..stream::Pull::default()
    });
    *result = EngineScenePull {
        flags: p.flags,
        epoch: p.epoch,
        first_media_frame: p.first_media_frame,
        media_frames: p.media_frames,
        requested_frames: p.requested_frames,
    };
}

/// 0 attaches, 1 detaches; returns whether the call took effect (attach) or 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_attach(
    handle: *const EngineSceneStream,
    attach: u32,
) -> u32 {
    let Ok(s) = (unsafe { stream(handle) }) else {
        return 0;
    };
    if attach != 0 {
        u32::from(s.attach_output())
    } else {
        s.detach_output();
        0
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_attached(handle: *const EngineSceneStream) -> u32 {
    unsafe { stream(handle) }.map_or(0, |s| u32::from(s.output_attached()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_status(
    handle: *const EngineSceneStream,
    out: *mut EngineSceneStatus,
) {
    let (Ok(s), Some(out)) = (unsafe { stream(handle) }, unsafe { out.as_mut() }) else {
        return;
    };
    let v = s.status();
    *out = EngineSceneStatus {
        state: v.state as u32,
        epoch: v.epoch,
        generation: v.generation,
        queued_input_samples: v.queued_input_samples,
        queued_input_bytes: v.queued_input_bytes,
        buffered_output_frames: v.buffered_output_frames,
        media_frames_pulled: v.media_frames_pulled,
        underruns: v.underruns,
        semantic_degradations: v.semantic_degradations,
        semantic_policy_revision: v.semantic_policy_revision,
        ring_fill: v.ring_fill,
        ended: u32::from(v.ended),
        failed: u32::from(v.failed),
        production_complete: u32::from(v.production_complete),
    };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_diagnostic_count(
    handle: *const EngineSceneStream,
) -> usize {
    unsafe { stream(handle) }.map_or(0, Stream::diagnostic_count)
}

/// Diagnostic `index`: 0 when it exists. Its text is copied up to `capacity` bytes; the record
/// carries the full length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_diagnostic(
    handle: *const EngineSceneStream,
    index: usize,
    out: *mut EngineSceneDiagnostic,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    let (Ok(s), Some(out)) = (unsafe { stream(handle) }, unsafe { out.as_mut() }) else {
        return 1;
    };
    let Some(d) = s.diagnostic(index) else {
        return 1;
    };
    *out = EngineSceneDiagnostic {
        level: d.level,
        code: d.code,
        epoch: d.epoch,
        generation: d.generation,
        element: d.element,
        field: d.field,
        message_len: d.message.len(),
    };
    if !message.is_null() {
        let n = d.message.len().min(capacity);
        unsafe { ptr::copy_nonoverlapping(d.message.as_ptr(), message, n) };
    }
    0
}

/// The manual replay clock's time in nanoseconds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_scene_set_clock(handle: *const EngineSceneStream, ns: i64) {
    if let Ok(s) = unsafe { stream(handle) } {
        s.set_clock(ns);
    }
}
