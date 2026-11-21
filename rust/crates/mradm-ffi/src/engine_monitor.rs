//! The realtime monitor (`mradm-engine` `monitor`) behind the C++
//! `MonitorEngine` shell, which keeps the device. A monitor takes over either a Rust file stream
//! (`mradm_engine_stream_*`, driven directly) or a C++ stream through the island table below;
//! the C++ trampolines catch every exception and report errors as codes and texts.
use super::*;
use mradm_engine::monitor::{Cancel, OutputStage, Overrides, Source, Validate};
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

/// Writes `message` (and an empty context) into an island call's error buffers as a C++ island
/// does: truncated to the buffer, NUL-terminated, its length stored; returns `code`.
pub(crate) unsafe fn island_error(e: *const EngineIslandError, code: i32, message: &str) -> i32 {
    unsafe { island_error_context(e, code, message, "") }
}

pub(crate) unsafe fn island_error_context(
    e: *const EngineIslandError,
    code: i32,
    message: &str,
    context: &str,
) -> i32 {
    unsafe fn write(text: &[u8], out: *mut u8, len: *mut usize) {
        let Some(len) = (unsafe { len.as_mut() }) else {
            return;
        };
        let n = if *len == 0 || out.is_null() {
            0
        } else {
            text.len().min(*len - 1)
        };
        if !out.is_null() && *len != 0 {
            unsafe {
                std::ptr::copy_nonoverlapping(text.as_ptr(), out, n);
                *out.add(n) = 0;
            }
        }
        *len = n;
    }
    if let Some(e) = unsafe { e.as_ref() } {
        unsafe {
            write(message.as_bytes(), e.message, e.message_len);
            write(context.as_bytes(), e.context, e.context_len);
        }
    }
    code
}
