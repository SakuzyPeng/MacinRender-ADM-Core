//! The realtime monitor (`mradm-engine` `monitor`) behind the C++
//! `MonitorEngine` shell, which keeps the device. A monitor takes over either a Rust file stream
//! (`mradm_engine_stream_*`, driven directly) or a C++ stream through the island table below;
//! the C++ trampolines catch every exception and report errors as codes and texts.
use super::room_ffi::{RenderRoomControl, control_handle};
use super::stream_ffi::{EngineStream, EngineStreamInfo};
use super::*;
use crate::engine_render::FfiHost;
use mradm_engine::backend::Host as _;
use mradm_engine::monitor::{
    Cancel, CreateError, LEVEL_CHANNELS, Monitor, MonitorHost, Native, ObjectOverride, OutputStage,
    Overrides, Receipt, Source, Validate,
};
use mradm_engine::stream::Info;
use std::ffi::c_void;
use std::sync::Arc;

/// The error buffers of one island call; `*_len` hold the capacity in and the length out.
#[repr(C)]
pub struct EngineIslandError {
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
}

/// A C++ stream (the Apple backend). `validate` and `cancel_request` may run on any thread,
/// `render_output` / `loop_render_reset` on the device callback; everything else on the worker.
/// `cancel_begin` starts a fresh stop source for the next `seek_cancellable`.
#[repr(C)]
pub struct EngineIslandStream {
    ctx: *mut c_void,
    out_channels: u32,
    sample_rate: u32,
    requires_preroll: u32,
    /// Nonzero for an output-stage stream: the intermediate width.
    intermediate_channels: u32,
    /// `renders_orientation_at_output`, which a session's backend switch compares.
    renders_orientation: u32,
    process: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut f32,
            usize,
            usize,
            *mut usize,
            *const EngineIslandError,
        ) -> i32,
    >,
    seek: Option<unsafe extern "C" fn(*mut c_void, u64, *const EngineIslandError) -> i32>,
    cancel_begin: Option<unsafe extern "C" fn(*mut c_void)>,
    cancel_request: Option<unsafe extern "C" fn(*mut c_void)>,
    seek_cancellable:
        Option<unsafe extern "C" fn(*mut c_void, u64, *const EngineIslandError) -> i32>,
    validate: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const RenderLiveOverride,
            usize,
            u64,
            *const EngineIslandError,
        ) -> i32,
    >,
    set_overrides: Option<unsafe extern "C" fn(*mut c_void, *const RenderLiveOverride, usize, u64)>,
    set_orientation: Option<unsafe extern "C" fn(*mut c_void, f32, f32, f32)>,
    produce_intermediate: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut f32,
            usize,
            usize,
            *mut usize,
            *const EngineIslandError,
        ) -> i32,
    >,
    render_output: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const f32,
            usize,
            *mut f32,
            usize,
            usize,
            f32,
            f32,
            f32,
        ) -> usize,
    >,
    reposition_source:
        Option<unsafe extern "C" fn(*mut c_void, u64, *const EngineIslandError) -> i32>,
    loop_render_reset: Option<unsafe extern "C" fn(*mut c_void, u64)>,
    render_failed: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

/// The shell's log sink and device controls.
#[repr(C)]
pub struct EngineLiveMonitorHost {
    ctx: *mut c_void,
    log: Option<unsafe extern "C" fn(*mut c_void, u32, *const u8, usize, *const u8, usize)>,
    device_flush: Option<unsafe extern "C" fn(*mut c_void)>,
    device_pause: Option<unsafe extern "C" fn(*mut c_void)>,
    device_resume: Option<unsafe extern "C" fn(*mut c_void)>,
}

/// A source for create or switch: a Rust stream handle (taken over) with its info, else the island.
#[repr(C)]
pub struct EngineLiveMonitorSource {
    stream: *mut EngineStream,
    info: EngineStreamInfo,
    island: *const EngineIslandStream,
}

/// `MonitorStatus`; `state`: 0 stopped, 1 playing, 2 paused.
#[repr(C)]
pub struct EngineLiveMonitorStatus {
    pub state: u32,
    pub playhead_frames: u64,
    pub underruns: u64,
    pub buffered_frames: u64,
    pub ring_fill: f32,
    pub ended: u32,
    pub failed: u32,
    pub override_revision: u64,
}

/// `MonitorLevels`.
#[repr(C)]
pub struct EngineLiveMonitorLevels {
    pub channels: u32,
    pub peak: [f32; LEVEL_CHANNELS],
    pub rms: [f32; LEVEL_CHANNELS],
    pub momentary_lufs: f32,
    pub shortterm_lufs: f32,
    pub integrated_lufs: f32,
}

/// One callback's result: frames taken from the ring; `mark_end` asks the device to mark the end.
#[repr(C)]
pub struct EngineLiveMonitorPull {
    pub(super) frames: usize,
    pub(super) mark_end: u32,
}

pub struct EngineLiveMonitor {
    monitor: Monitor,
}

/// The C++ `StreamSwitchReceipt` of a switch: `finish` stores each state (1 applied, 2 rejected),
/// `release` drops the C++ reference once the monitor no longer holds the receipt.
#[repr(C)]
pub struct EngineLiveMonitorReceipt {
    ctx: *mut c_void,
    finish: Option<unsafe extern "C" fn(*mut c_void, u32)>,
    release: Option<unsafe extern "C" fn(*mut c_void)>,
}

struct ShellReceipt(EngineLiveMonitorReceipt);
// SAFETY: The C++ receipt is an atomic behind a shared_ptr the context owns.
unsafe impl Send for ShellReceipt {}
unsafe impl Sync for ShellReceipt {}

impl Drop for ShellReceipt {
    fn drop(&mut self) {
        if let Some(release) = self.0.release {
            unsafe { release(self.0.ctx) };
        }
    }
}

/// The C++ code a failed `MonitorEngine` constructor threw with (the shell rethrows).
const CONSTRUCTION_FAILED: i32 = mradm_engine::monitor::session::THROWN;
const ISLAND_TEXT: usize = 4096;

struct Island {
    table: EngineIslandStream,
}
// SAFETY: The C++ stream behind the table is used by the protocol above: worker-only calls from
// one thread at a time, and the documented thread-safe entries elsewhere.
unsafe impl Send for Island {}
unsafe impl Sync for Island {}

impl Drop for Island {
    fn drop(&mut self) {
        if let Some(destroy) = self.table.destroy {
            unsafe { destroy(self.table.ctx) };
        }
    }
}

pub(super) struct Buffers {
    message: Vec<u8>,
    context: Vec<u8>,
}

impl Buffers {
    pub(super) fn new() -> Self {
        Self {
            message: vec![0; ISLAND_TEXT],
            context: vec![0; ISLAND_TEXT],
        }
    }

    /// Runs one island call with the buffers and converts its status.
    pub(super) fn call(
        &mut self,
        f: impl FnOnce(*const EngineIslandError) -> i32,
    ) -> Result<(), Error> {
        let (mut message_len, mut context_len) = (self.message.len(), self.context.len());
        let e = EngineIslandError {
            message: self.message.as_mut_ptr(),
            message_len: &mut message_len,
            context: self.context.as_mut_ptr(),
            context_len: &mut context_len,
        };
        let code = f(&e);
        if code == 0 {
            return Ok(());
        }
        let text = |b: &[u8], n: usize| b[..n.min(b.len().saturating_sub(1))].to_vec();
        Err(Error {
            code,
            message: text(&self.message, message_len),
            context: text(&self.context, context_len),
        })
    }
}

/// The C records of owned overrides (they borrow `overrides`).
fn records(overrides: &Overrides) -> Vec<RenderLiveOverride> {
    let text = |b: &[u8]| Text {
        data: b.as_ptr(),
        len: b.len(),
    };
    overrides
        .objects
        .iter()
        .map(|o| RenderLiveOverride {
            base: RenderOverride {
                object_id: text(&o.object_id),
                speaker_label: text(&o.speaker_label),
                mute: u32::from(o.mute),
                gain_db: o.gain_db,
                has_head_locked: u32::from(o.head_locked.is_some()),
                head_locked: u32::from(o.head_locked == Some(true)),
            },
            extent_scale: o.extent_scale,
            extent_width_scale: o.extent_width_scale,
            extent_height_scale: o.extent_height_scale,
            extent_depth_scale: o.extent_depth_scale,
            diffuse_scale: o.diffuse_scale,
            divergence_scale: o.divergence_scale,
        })
        .collect()
}

pub(super) struct IslandSource {
    island: Arc<Island>,
    buffers: Buffers,
    stage: Option<Arc<IslandStage>>,
}

struct IslandValidator(Arc<Island>);

impl Validate for IslandValidator {
    fn validate(&self, overrides: &Overrides) -> Result<(), Error> {
        let Some(f) = self.0.table.validate else {
            return Ok(());
        };
        let values = records(overrides);
        let revision = overrides.revision;
        Buffers::new()
            .call(|e| unsafe { f(self.0.table.ctx, values.as_ptr(), values.len(), revision, e) })
    }
}

struct IslandStage(Arc<Island>);

impl OutputStage for IslandStage {
    fn render_output(
        &self,
        intermediate: &[f32],
        output: &mut [f32],
        frames: usize,
        pose: [f32; 3],
    ) -> usize {
        self.0.table.render_output.map_or(0, |f| unsafe {
            f(
                self.0.table.ctx,
                intermediate.as_ptr(),
                intermediate.len(),
                output.as_mut_ptr(),
                output.len(),
                frames,
                pose[0],
                pose[1],
                pose[2],
            )
        })
    }
    fn loop_render_reset(&self, frame: u64) {
        if let Some(f) = self.0.table.loop_render_reset {
            unsafe { f(self.0.table.ctx, frame) };
        }
    }
    fn render_failed(&self) -> bool {
        self.0
            .table
            .render_failed
            .is_some_and(|f| unsafe { f(self.0.table.ctx) } != 0)
    }
}

impl IslandSource {
    pub(super) fn new(table: EngineIslandStream) -> Self {
        let output_stage = table.intermediate_channels > 0;
        let island = Arc::new(Island { table });
        Self {
            stage: output_stage.then(|| Arc::new(IslandStage(island.clone()))),
            island,
            buffers: Buffers::new(),
        }
    }

    fn t(&self) -> &EngineIslandStream {
        &self.island.table
    }

    fn produce(
        &mut self,
        f: Option<
            unsafe extern "C" fn(
                *mut c_void,
                *mut f32,
                usize,
                usize,
                *mut usize,
                *const EngineIslandError,
            ) -> i32,
        >,
        output: &mut [f32],
        frames: usize,
    ) -> Result<usize, Error> {
        let Some(f) = f else {
            return Ok(0);
        };
        let ctx = self.island.table.ctx;
        let mut produced = 0;
        self.buffers.call(|e| unsafe {
            f(
                ctx,
                output.as_mut_ptr(),
                output.len(),
                frames,
                &mut produced,
                e,
            )
        })?;
        Ok(produced)
    }

    fn seek_call(
        &mut self,
        f: Option<unsafe extern "C" fn(*mut c_void, u64, *const EngineIslandError) -> i32>,
        frame: u64,
    ) -> Result<(), Error> {
        let Some(f) = f else {
            return Ok(());
        };
        let ctx = self.island.table.ctx;
        self.buffers.call(|e| unsafe { f(ctx, frame, e) })
    }
}

impl Source for IslandSource {
    fn out_channels(&self) -> usize {
        self.t().out_channels as usize
    }
    fn sample_rate(&self) -> u32 {
        self.t().sample_rate
    }
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        let f = self.t().process;
        self.produce(f, output, frames)
    }
    fn seek(&mut self, frame: u64) -> Result<(), Error> {
        let f = self.t().seek;
        self.seek_call(f, frame)
    }
    fn seek_with_cancel(&mut self, frame: u64, cancel: &Cancel) -> Result<(), Error> {
        let (ctx, begin, request) = (self.t().ctx, self.t().cancel_begin, self.t().cancel_request);
        if let Some(begin) = begin {
            unsafe { begin(ctx) };
        }
        let island = self.island.clone();
        let _watch = cancel.watch(Box::new(move || {
            if let Some(request) = request {
                unsafe { request(island.table.ctx) };
            }
        }));
        let f = self.t().seek_cancellable;
        self.seek_call(f, frame)
    }
    fn seek_requires_preroll(&self) -> bool {
        self.t().requires_preroll != 0
    }
    fn validator(&self) -> Arc<dyn Validate> {
        Arc::new(IslandValidator(self.island.clone()))
    }
    fn set_overrides(&mut self, overrides: &Overrides) {
        if let Some(f) = self.t().set_overrides {
            let values = records(overrides);
            unsafe {
                f(
                    self.t().ctx,
                    values.as_ptr(),
                    values.len(),
                    overrides.revision,
                )
            };
        }
    }
    fn set_orientation(&mut self, pose: [f32; 3]) {
        if let Some(f) = self.t().set_orientation {
            unsafe { f(self.t().ctx, pose[0], pose[1], pose[2]) };
        }
    }
    fn output_stage(&self) -> Option<Arc<dyn OutputStage>> {
        self.stage.clone().map(|s| s as Arc<dyn OutputStage>)
    }
    fn renders_orientation_at_output(&self) -> bool {
        self.t().renders_orientation != 0
    }
    fn intermediate_channels(&self) -> usize {
        self.t().intermediate_channels as usize
    }
    fn produce_intermediate(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        let f = self.t().produce_intermediate;
        self.produce(f, output, frames)
    }
    fn reposition_source(&mut self, frame: u64) -> Result<(), Error> {
        let f = self.t().reposition_source;
        self.seek_call(f, frame)
    }
}

struct ShellHost {
    table: EngineLiveMonitorHost,
}
// SAFETY: The C++ log sink and device controls are thread-safe (the engine called them from the
// worker and the control thread).
unsafe impl Send for ShellHost {}
unsafe impl Sync for ShellHost {}

impl MonitorHost for ShellHost {
    fn log(&self, level: u32, module: &str, message: &[u8]) {
        if let Some(f) = self.table.log {
            unsafe {
                f(
                    self.table.ctx,
                    level,
                    module.as_ptr(),
                    module.len(),
                    message.as_ptr(),
                    message.len(),
                )
            };
        }
    }
    fn device_flush(&self) {
        if let Some(f) = self.table.device_flush {
            unsafe { f(self.table.ctx) };
        }
    }
    fn device_pause(&self) {
        if let Some(f) = self.table.device_pause {
            unsafe { f(self.table.ctx) };
        }
    }
    fn device_resume(&self) {
        if let Some(f) = self.table.device_resume {
            unsafe { f(self.table.ctx) };
        }
    }
    fn room_worker(&self, active: bool) {
        FfiHost.room_worker(active);
    }
    fn room_wait(&self, duration: std::time::Duration) {
        FfiHost.room_wait(duration);
    }
}

/// Takes over the source: the Rust stream handle is consumed, the island table copied.
unsafe fn source(s: *const EngineLiveMonitorSource) -> Result<Box<dyn Source>, Error> {
    let s = unsafe { s.as_ref() }.ok_or_else(|| bad("无效监听流"))?;
    if !s.stream.is_null() {
        let stream = unsafe { Box::from_raw(s.stream) };
        let (validator, body) = stream.into_parts();
        let info = Info {
            outputs: s.info.outputs,
            sample_rate: s.info.sample_rate,
            binaural: s.info.binaural != 0,
            requires_preroll: s.info.requires_preroll != 0,
        };
        return Ok(Box::new(Native::new(validator, body, info)));
    }
    let island = unsafe { s.island.as_ref() }.ok_or_else(|| bad("无效监听流"))?;
    Ok(Box::new(IslandSource::new(EngineIslandStream {
        ..*island
    })))
}

unsafe fn monitor<'a>(handle: *const EngineLiveMonitor) -> Option<&'a Monitor> {
    unsafe { handle.as_ref() }.map(|h| &h.monitor)
}

/// Creates the monitor and starts its worker; on failure the source is still consumed. Code 100:
/// the C++ constructor threw (the shell rethrows the message).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_create(
    source: *const EngineLiveMonitorSource,
    realtime: u32,
    host: *const EngineLiveMonitorHost,
    out: *mut *mut EngineLiveMonitor,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let out = unsafe { out.as_mut() }.ok_or_else(|| bad("无效输出指针"))?;
        *out = ptr::null_mut();
        let host = unsafe { host.as_ref() }.ok_or_else(|| bad("无效监听宿主"))?;
        let source = unsafe { self::source(source)? };
        let host = Arc::new(ShellHost {
            table: EngineLiveMonitorHost { ..*host },
        });
        match Monitor::create(source, realtime != 0, host) {
            Ok(monitor) => {
                *out = Box::into_raw(Box::new(EngineLiveMonitor { monitor }));
                Ok(())
            }
            Err(CreateError::Error(e)) => Err(e),
            Err(CreateError::Construction(message)) => Err(Error {
                code: CONSTRUCTION_FAILED,
                message: message.into_bytes(),
                context: Vec::new(),
            }),
        }
    })
}

/// The first half of the shutdown, before the shell stops the device.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_begin_shutdown(
    handle: *const EngineLiveMonitor,
) {
    if let Some(m) = unsafe { monitor(handle) } {
        m.begin_shutdown();
    }
}

/// The device stopped: joins the worker and frees the monitor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_destroy(handle: *mut EngineLiveMonitor) {
    if !handle.is_null() {
        let mut h = unsafe { Box::from_raw(handle) };
        h.monitor.shutdown();
    }
}

/// (channels, sample rate, output stage) of the monitor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_format(
    handle: *const EngineLiveMonitor,
    channels: *mut u32,
    sample_rate: *mut u32,
    output_stage: *mut u32,
) {
    let Some(m) = (unsafe { monitor(handle) }) else {
        return;
    };
    unsafe {
        if let Some(c) = channels.as_mut() {
            *c = m.channels() as u32;
        }
        if let Some(r) = sample_rate.as_mut() {
            *r = m.sample_rate();
        }
        if let Some(o) = output_stage.as_mut() {
            *o = u32::from(m.output_stage());
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_play(handle: *const EngineLiveMonitor) {
    if let Some(m) = unsafe { monitor(handle) } {
        m.play();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_pause(handle: *const EngineLiveMonitor) {
    if let Some(m) = unsafe { monitor(handle) } {
        m.pause();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_seek(
    handle: *const EngineLiveMonitor,
    frame: u64,
) {
    if let Some(m) = unsafe { monitor(handle) } {
        m.seek(frame);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_set_loop(
    handle: *const EngineLiveMonitor,
    start: u64,
    end: u64,
) {
    if let Some(m) = unsafe { monitor(handle) } {
        m.set_loop(start, end);
    }
}

/// Validates and queues the live edits (`MonitorEngine::set_overrides`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_overrides(
    handle: *const EngineLiveMonitor,
    values: *const RenderLiveOverride,
    values_len: usize,
    revision: u64,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let m = unsafe { monitor(handle) }.ok_or_else(|| bad("无效监听句柄"))?;
        m.set_overrides(unsafe { overrides_of(values, values_len, revision)? })
    })
}

/// Owned live edits from override records.
pub(super) unsafe fn overrides_of(
    values: *const RenderLiveOverride,
    values_len: usize,
    revision: u64,
) -> Result<Overrides, Error> {
    let records = unsafe { input(values, values_len)? };
    let mut objects = Vec::with_capacity(records.len());
    for o in records {
        let base = unsafe { override_value(&o.base)? };
        objects.push(ObjectOverride {
            object_id: base.object_id.to_vec(),
            speaker_label: base.speaker_label.to_vec(),
            mute: base.mute,
            gain_db: base.gain_db,
            head_locked: base.head_locked,
            extent_scale: o.extent_scale,
            extent_width_scale: o.extent_width_scale,
            extent_height_scale: o.extent_height_scale,
            extent_depth_scale: o.extent_depth_scale,
            diffuse_scale: o.diffuse_scale,
            divergence_scale: o.divergence_scale,
        });
    }
    Ok(Overrides { objects, revision })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_orientation(
    handle: *const EngineLiveMonitor,
    yaw: f32,
    pitch: f32,
    roll: f32,
) {
    if let Some(m) = unsafe { monitor(handle) } {
        m.set_orientation([yaw, pitch, roll]);
    }
}

/// Queues a hot-switch to the source (consumed). `receipt` may be null; the monitor releases it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_switch(
    handle: *const EngineLiveMonitor,
    source: *const EngineLiveMonitorSource,
    receipt: *const EngineLiveMonitorReceipt,
) {
    let receipt = unsafe { receipt.as_ref() }.map(|r| {
        let shell = Arc::new(ShellReceipt(EngineLiveMonitorReceipt { ..*r }));
        Arc::new(Receipt::observed(Box::new(move |state| {
            if let Some(finish) = shell.0.finish {
                unsafe { finish(shell.0.ctx, u32::from(state)) };
            }
        })))
    });
    let (Some(m), Ok(source)) = (unsafe { monitor(handle) }, unsafe { self::source(source) })
    else {
        if let Some(r) = &receipt {
            drop(r.clone());
        }
        return;
    };
    m.switch(source, receipt);
}

/// The monitor's HpTF processor (`mradm_dsp_hptf_live_*`), valid until the monitor is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_hptf(
    handle: *const EngineLiveMonitor,
) -> *const mradm_dsp::hptf::Live {
    unsafe { monitor(handle) }.map_or(ptr::null(), |m| m.hptf() as *const _)
}

/// The current stream's room control, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_room_control(
    handle: *const EngineLiveMonitor,
) -> *mut RenderRoomControl {
    unsafe { monitor(handle) }
        .and_then(Monitor::room_control)
        .map_or(ptr::null_mut(), control_handle)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_status(
    handle: *const EngineLiveMonitor,
    out: *mut EngineLiveMonitorStatus,
) {
    let (Some(m), Some(out)) = (unsafe { monitor(handle) }, unsafe { out.as_mut() }) else {
        return;
    };
    let s = m.status();
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
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_levels(
    handle: *const EngineLiveMonitor,
    out: *mut EngineLiveMonitorLevels,
) {
    let (Some(m), Some(out)) = (unsafe { monitor(handle) }, unsafe { out.as_mut() }) else {
        return;
    };
    let l = m.levels();
    *out = EngineLiveMonitorLevels {
        channels: l.channels,
        peak: l.peak,
        rms: l.rms,
        momentary_lufs: l.momentary_lufs,
        shortterm_lufs: l.shortterm_lufs,
        integrated_lufs: l.integrated_lufs,
    };
}

/// Device callback: never waits or allocates. `output` holds `output_len` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_live_monitor_pull(
    handle: *const EngineLiveMonitor,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    result: *mut EngineLiveMonitorPull,
) {
    let (Some(m), Some(result)) = (unsafe { monitor(handle) }, unsafe { result.as_mut() }) else {
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
    *result = EngineLiveMonitorPull {
        frames: pulled.frames,
        mark_end: u32::from(pulled.mark_end),
    };
}

/// Writes `message` into an island call's error buffers, as a C++ island does.
#[cfg(test)]
pub(crate) unsafe fn island_error_for_test(
    e: *const EngineIslandError,
    code: i32,
    message: &str,
) -> i32 {
    let e = unsafe { &*e };
    let n = message
        .len()
        .min(unsafe { *e.message_len }.saturating_sub(1));
    unsafe {
        std::ptr::copy_nonoverlapping(message.as_ptr(), e.message, n);
        *e.message.add(n) = 0;
        *e.message_len = n;
        *e.context_len = 0;
    }
    code
}
