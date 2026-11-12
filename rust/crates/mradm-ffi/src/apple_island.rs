//! The Apple backend's islands (`src/adm_apple`, `MradmAppleIslands`): Rust builds their settings
//! from the request, keeps the preparation and calls them for the offline render (islands 2 and
//! 3) and the monitor's stream.
use super::engine_monitor::{Buffers, EngineIslandError, EngineIslandStream, IslandSource};
use crate::adm::Text;
use crate::engine_service::{EngineIslandReply, IslandContext, empty_reply};
use crate::scene::{DirectMatrix, DirectMatrixView, SceneHandle};
use mradm_engine::monitor::Source;
use mradm_engine::render::{AppleOptions, PathText};
use mradm_render::offline::Window;
use mradm_render::routing::Error;
use mradm_scene::{RenderScene, direct_matrix};
use std::ffi::c_void;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::Arc;

/// The `RenderPlan` fields the Apple backend reads; the values are those of the request record.
#[repr(C)]
pub struct AppleSettings {
    input: Text,
    layout: Text,
    routing: u32,
    has_matrix: u32,
    matrix: DirectMatrixView,
    split_power: u32,
    speaker_spread: u32,
    binaural_spread: u32,
    spatial_preset: u32,
    speaker_rendering_flags: u32,
    orientation: [f32; 3],
}

/// The Apple island's preparation.
pub struct ApplePrepared {
    _opaque: [u8; 0],
}

/// The Apple backend's entries. `prepare` takes the scene; reply texts live until the next call
/// on the thread; `open_stream` returns an error code (100 for an exception).
#[repr(C)]
pub struct AppleIslands {
    prepare: Option<
        unsafe extern "C" fn(
            *const AppleSettings,
            *mut SceneHandle,
            *const IslandContext,
            *mut *mut ApplePrepared,
            *mut EngineIslandReply,
        ) -> i32,
    >,
    render: Option<
        unsafe extern "C" fn(
            *const ApplePrepared,
            *const AppleSettings,
            Text,
            u32,
            u64,
            u64,
            *const IslandContext,
            *mut EngineIslandReply,
        ) -> i32,
    >,
    open_stream: Option<
        unsafe extern "C" fn(
            *const ApplePrepared,
            *const AppleSettings,
            *const IslandContext,
            *mut EngineIslandStream,
            *const EngineIslandError,
        ) -> i32,
    >,
    destroy: Option<unsafe extern "C" fn(*mut ApplePrepared)>,
}

/// A caller's table (static in the Apple island).
#[derive(Clone, Copy)]
pub(crate) struct Apple(*const AppleIslands);
// SAFETY: The table is immutable process data and its entries are thread-safe.
unsafe impl Send for Apple {}
unsafe impl Sync for Apple {}

impl Apple {
    /// The table, or `None` for null.
    pub(crate) fn new(table: *const AppleIslands) -> Option<Self> {
        (!table.is_null()).then_some(Self(table))
    }

    fn table(&self) -> &AppleIslands {
        unsafe { &*self.0 }
    }

    /// Prepares over a copy of `scene` (the island takes it). The preparation is `Some` only on
    /// success; `reply` holds any failure.
    pub(crate) fn prepare(
        &self,
        settings: &Settings<'_>,
        scene: Option<&RenderScene>,
        context: &IslandContext,
        reply: &mut EngineIslandReply,
    ) -> Option<Arc<Preparation>> {
        let Some(prepare) = self.table().prepare else {
            unavailable(reply);
            return None;
        };
        let scene = scene.map_or(ptr::null_mut(), |s| {
            Box::into_raw(Box::new(SceneHandle::new(s.clone())))
        });
        let mut raw = ptr::null_mut();
        if unsafe { prepare(&settings.record, scene, context, &mut raw, reply) } != 0 {
            unavailable(reply);
        }
        let prepared = (!raw.is_null()).then(|| Arc::new(Preparation { apple: *self, raw }));
        prepared.filter(|_| reply.code == 0)
    }

    /// Renders `output` over `window`; the reply carries the metrics.
    pub(crate) fn render(
        &self,
        prepared: &Preparation,
        settings: &Settings<'_>,
        output: &str,
        window: Option<Window>,
        context: &IslandContext,
        reply: &mut EngineIslandReply,
    ) {
        let Some(render) = self.table().render else {
            return unavailable(reply);
        };
        let w = window.unwrap_or(Window { start: 0, count: 0 });
        let code = unsafe {
            render(
                prepared.raw,
                &settings.record,
                Text::new(output),
                u32::from(window.is_some()),
                w.start,
                w.count,
                context,
                reply,
            )
        };
        if code != 0 {
            unavailable(reply);
        }
    }

    /// The monitor's stream over `prepared` (which the stream keeps).
    pub(crate) fn open_stream(
        &self,
        prepared: &Preparation,
        settings: &Settings<'_>,
        context: &IslandContext,
    ) -> Result<Box<dyn Source>, Error> {
        let Some(open) = self.table().open_stream else {
            return Err(Error {
                code: 6,
                message: UNAVAILABLE.as_bytes().to_vec(),
                context: Vec::new(),
            });
        };
        let mut island = MaybeUninit::<EngineIslandStream>::zeroed();
        Buffers::new().call(|e| unsafe {
            open(
                prepared.raw,
                &settings.record,
                context,
                island.as_mut_ptr(),
                e,
            )
        })?;
        Ok(Box::new(IslandSource::new(unsafe { island.assume_init() })))
    }
}

const UNAVAILABLE: &str = "the Apple backend is not available in this build";

fn unavailable(reply: &mut EngineIslandReply) {
    *reply = empty_reply();
    reply.code = 6;
    reply.message = Text::new(UNAVAILABLE);
}

/// An Apple preparation, destroyed with its table's `destroy`.
pub(crate) struct Preparation {
    apple: Apple,
    raw: *mut ApplePrepared,
}
// SAFETY: The preparation is immutable after `prepare`; the island shares it between renders and
// streams on any thread.
unsafe impl Send for Preparation {}
unsafe impl Sync for Preparation {}

impl Drop for Preparation {
    fn drop(&mut self) {
        if let Some(destroy) = self.apple.table().destroy {
            unsafe { destroy(self.raw) };
        }
    }
}

/// The settings record of one call and the matrix it points at.
pub(crate) struct Settings<'a> {
    record: AppleSettings,
    _matrix: Option<Box<DirectMatrix>>,
    _borrows: PhantomData<&'a str>,
}

impl<'a> Settings<'a> {
    pub(crate) fn new(
        input: &'a PathText,
        layout: &'a str,
        matrix: Option<&direct_matrix::Matrix>,
        o: &AppleOptions,
    ) -> Self {
        let matrix = matrix.map(|m| DirectMatrix::new(m.clone()));
        Self {
            record: AppleSettings {
                input: Text::new(&input.utf8),
                layout: Text::new(layout),
                routing: o.routing,
                has_matrix: u32::from(matrix.is_some()),
                matrix: matrix
                    .as_ref()
                    .map_or_else(DirectMatrix::empty_view, |m| m.view()),
                split_power: u32::from(o.split_power),
                speaker_spread: o.speaker_spread,
                binaural_spread: o.binaural_spread,
                spatial_preset: o.spatial_preset,
                speaker_rendering_flags: u32::from(o.speaker_rendering_flags),
                orientation: o.orientation,
            },
            _matrix: matrix,
            _borrows: PhantomData,
        }
    }
}

/// A context forwarding the island's log lines to `log` (the monitor session's buffer).
pub(crate) fn log_context(log: &&dyn Fn(u32, &str, &[u8])) -> IslandContext {
    unsafe extern "C" fn forward(user: *mut c_void, level: u32, module: Text, message: Text) {
        let log = unsafe { &*user.cast::<&dyn Fn(u32, &str, &[u8])>() };
        let bytes = |t: &Text| {
            if t.data.is_null() {
                &[][..]
            } else {
                unsafe { std::slice::from_raw_parts(t.data, t.len) }
            }
        };
        log(
            level,
            std::str::from_utf8(bytes(&module)).unwrap_or(""),
            bytes(&message),
        );
    }
    IslandContext {
        user: ptr::from_ref(log).cast_mut().cast(),
        cancelled: None,
        progress: None,
        log: Some(forward),
    }
}

/// The fields of a settings record, for tests.
#[cfg(test)]
#[derive(Debug)]
#[allow(dead_code)] // Read through Debug.
pub(crate) struct AppleFields {
    input: String,
    pub(crate) layout: String,
    routing: u32,
    has_matrix: u32,
    split_power: u32,
    speaker_spread: u32,
    binaural_spread: u32,
    spatial_preset: u32,
    speaker_rendering_flags: u32,
    orientation: [f32; 3],
}

#[cfg(test)]
pub(crate) fn settings_fields(s: &AppleSettings) -> AppleFields {
    let text = |t: &Text| String::from_utf8(unsafe { super::bytes(t) }.unwrap().to_vec()).unwrap();
    AppleFields {
        input: text(&s.input),
        layout: text(&s.layout),
        routing: s.routing,
        has_matrix: s.has_matrix,
        split_power: s.split_power,
        speaker_spread: s.speaker_spread,
        binaural_spread: s.binaural_spread,
        spatial_preset: s.spatial_preset,
        speaker_rendering_flags: s.speaker_rendering_flags,
        orientation: s.orientation,
    }
}

/// A table of test entries.
#[cfg(test)]
pub(crate) type PrepareFn = unsafe extern "C" fn(
    *const AppleSettings,
    *mut SceneHandle,
    *const IslandContext,
    *mut *mut ApplePrepared,
    *mut EngineIslandReply,
) -> i32;
#[cfg(test)]
pub(crate) type RenderFn = unsafe extern "C" fn(
    *const ApplePrepared,
    *const AppleSettings,
    Text,
    u32,
    u64,
    u64,
    *const IslandContext,
    *mut EngineIslandReply,
) -> i32;
#[cfg(test)]
pub(crate) type OpenFn = unsafe extern "C" fn(
    *const ApplePrepared,
    *const AppleSettings,
    *const IslandContext,
    *mut EngineIslandStream,
    *const EngineIslandError,
) -> i32;

#[cfg(test)]
pub(crate) const fn test_table(
    prepare: Option<PrepareFn>,
    render: Option<RenderFn>,
    open_stream: Option<OpenFn>,
    destroy: Option<unsafe extern "C" fn(*mut ApplePrepared)>,
) -> AppleIslands {
    AppleIslands {
        prepare,
        render,
        open_stream,
        destroy,
    }
}

// SAFETY: A table of function pointers.
unsafe impl Sync for AppleIslands {}
