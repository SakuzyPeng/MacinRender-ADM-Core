//! Private boundary of the offline render service (`mradm-engine`
//! `render`): the C++ `RenderService` steps a handle; the islands a registered runner accepts run
//! inside the step, the others are returned and answered. Texts in events and results live until
//! the next call on the handle.
use crate::adm::Text;
use crate::engine_render::FfiHost;
use crate::render::RenderPrepared;
use crate::render::apple_island::{Apple, AppleIslands, Preparation, Settings};
use crate::scene::{DirectMatrix, DirectMatrixView, SceneHandle};
use mradm_engine::offline::{Metrics, Progress};
use mradm_engine::render::{self, Event, Island, Outcome, PathText, Render, Reply, Request, Reuse};
use mradm_engine::request::{self, Packager};
use mradm_scene::RenderScene;
use std::{
    cell::{Cell, RefCell},
    ffi::c_void,
    ptr, slice,
    sync::Arc,
};

/// A path's UTF-8 text and the caller's spelling of it in messages.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EnginePath {
    pub utf8: Text,
    pub display: Text,
}

/// A progress event (`ProgressEvent`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineProgress {
    pub stage: u32,
    pub operation: u32,
    pub fraction: f64,
    pub stage_fraction: f64,
    pub current: u64,
    pub total: u64,
    pub message: Text,
}

/// One event: `kind` 0 nothing, 1 progress, 2 log (`level`, `module`, `message`), 3 island, 4
/// done (`mradm_engine_render_result`). Islands: 1 MP4 packager probe, 2 Apple prepare, 3 Apple
/// render, 4 FLAC, 5 Opus MKA, 6 APAC, 7 IAMF, 8 MP4 packaging, 9 metadata; `src` / `dst` the
/// files, the Apple plan's output in `dst`.
#[repr(C)]
pub struct EngineRenderEvent {
    pub kind: u32,
    pub progress: EngineProgress,
    pub level: u32,
    pub module: Text,
    pub message: Text,
    pub island: u32,
    pub src: EnginePath,
    pub dst: EnginePath,
    pub layout: Text,
    pub bitrate: u32,
    pub drc_music: u32,
    pub caf: u32,
    pub has_lufs: u32,
    pub has_peak: u32,
    pub lufs: f64,
    pub peak_dbtp: f64,
    pub layers: *const Text,
    pub layers_len: usize,
    pub has_window: u32,
    pub window_start: u64,
    pub window_count: u64,
    pub has_meter_window: u32,
    pub meter_start: u64,
    pub meter_count: u64,
    pub has_matrix: u32,
    pub matrix: DirectMatrixView,
    pub encoder: Text,
    pub date_utc: Text,
    pub renderer: Text,
}

/// An island's answer: `code` 0 for success; the Apple render's metrics; the probed packager (0
/// none, 1 mp4box, 2 ffmpeg) and ffmpeg major version (-1 unknown).
#[repr(C)]
pub struct EngineIslandReply {
    pub code: i32,
    pub message: Text,
    pub context: Text,
    pub has_lufs: u32,
    pub has_peak: u32,
    pub lufs: f64,
    pub peak_dbtp: f64,
    pub packager: u32,
    pub ffmpeg_major: i32,
}

/// A render in progress over the islands of its runner.
pub struct EngineRender {
    render: Render,
    islands: Option<IslandRunner>,
    /// The Apple preparation the caller cached or this render made (then `apple_made`).
    apple_prepared: RefCell<Option<Arc<Preparation>>>,
    apple_made: Cell<bool>,
}

impl EngineRender {
    /// A render of `request`; `preimported` is a preview session's policy-applied scene and
    /// `cached` its earlier preparation (a Rust backend's or the Apple island's).
    fn create(
        request: Request,
        platform: mradm_engine::catalog::Platform,
        preimported: Option<&SceneHandle>,
        cached: Option<&RenderPrepared>,
    ) -> Box<Self> {
        let apple = cached.and_then(|c| c.apple.clone());
        let reuse = Reuse {
            scene: preimported.map(|scene| scene.scene().clone()),
            prepared: cached.and_then(|c| c.prepared.clone()),
            apple_prepared: apple.is_some(),
        };
        Box::new(Self {
            render: Render::new(request, platform, reuse),
            islands: None,
            apple_prepared: RefCell::new(apple),
            apple_made: Cell::new(false),
        })
    }

    /// One step, running the islands the registered runner accepts.
    fn step(&mut self, cancelled: bool) -> Event {
        let runner = self.islands.as_ref().map(|islands| Runner {
            islands,
            apple: Apple::new(islands.apple),
            prepared: &self.apple_prepared,
            made: &self.apple_made,
        });
        let islands = runner.as_ref().map(|r| r as &dyn render::Islands);
        self.render.next_with(cancelled, &FfiHost, islands)
    }

    /// The preparation the render made (a Rust backend's and the Apple island's), if any.
    fn take(&mut self) -> Option<Box<RenderPrepared>> {
        let prepared = self.render.take_prepared();
        let apple = self
            .apple_made
            .get()
            .then(|| self.apple_prepared.borrow().clone())
            .flatten();
        if prepared.is_none() && apple.is_none() {
            return None;
        }
        Some(Box::new(RenderPrepared { prepared, apple }))
    }
}

/// The offline render with its islands, for Rust callers (`mradm-capi`): the same steps the
/// `mradm_engine_render_*` entries expose to C, on Rust types.
pub struct RenderDriver(Box<EngineRender>);

impl RenderDriver {
    /// `islands` runs the audio islands and the Apple backend inside the steps.
    ///
    /// # Safety
    ///
    /// The callbacks must honor the island pointer, ownership and reply contracts. They must
    /// complete before their island call returns and must not retain its event, reply or context
    /// pointers. Data reached through `islands.driver.user` must remain valid until this driver is
    /// dropped and support every thread on which the islands invoke the callbacks.
    ///
    /// A non-null `islands.apple` must point to an immutable, thread-safe table that remains valid
    /// until this driver and every preparation returned by [`Self::take_prepared`] (including
    /// shared copies) have been dropped.
    ///
    /// ```compile_fail,E0133
    /// use mradm_engine::{catalog::Platform, render::Request};
    /// use mradm_ffi::api::render::{IslandRunner, RenderDriver};
    ///
    /// fn create(request: Request, platform: Platform, islands: IslandRunner) {
    ///     RenderDriver::new(request, platform, None, None, islands);
    /// }
    /// ```
    pub unsafe fn new(
        request: Request,
        platform: mradm_engine::catalog::Platform,
        preimported: Option<&SceneHandle>,
        cached: Option<&RenderPrepared>,
        islands: IslandRunner,
    ) -> Self {
        let mut render = EngineRender::create(request, platform, preimported, cached);
        render.islands = Some(islands);
        Self(render)
    }

    /// The next event; an island no runner accepted is returned to be answered with
    /// [`Self::resume`].
    pub fn next(&mut self, cancelled: bool) -> Event {
        self.0.step(cancelled)
    }

    pub fn resume(&mut self, reply: Reply) {
        self.0.render.resume(reply);
    }

    /// The result after [`Event::Done`].
    pub fn outcome(&self) -> Option<&Outcome> {
        self.0.render.outcome()
    }

    /// The preparation the render made, for a preview session's cache.
    pub fn take_prepared(&mut self) -> Option<Box<RenderPrepared>> {
        self.0.take()
    }
}

unsafe fn bytes<'a>(t: &Text) -> Option<&'a [u8]> {
    if t.len == 0 {
        return Some(&[]);
    }
    (!t.data.is_null()).then(|| unsafe { slice::from_raw_parts(t.data, t.len) })
}

unsafe fn string(t: &Text) -> Option<String> {
    unsafe { bytes(t) }.map(|b| String::from_utf8_lossy(b).into_owned())
}

fn progress_record(p: &Progress) -> EngineProgress {
    EngineProgress {
        stage: p.stage,
        operation: p.operation,
        fraction: p.fraction,
        stage_fraction: p.stage_fraction,
        current: p.current,
        total: p.total,
        message: Text::new(p.message),
    }
}

fn empty_path() -> EnginePath {
    EnginePath {
        utf8: Text::new(""),
        display: Text::new(""),
    }
}

fn path_record(p: &PathText) -> EnginePath {
    EnginePath {
        utf8: Text::new(&p.utf8),
        display: Text::new(&p.display),
    }
}

fn empty_event() -> EngineRenderEvent {
    EngineRenderEvent {
        kind: 0,
        progress: progress_record(&Progress {
            stage: 0,
            operation: 0,
            fraction: 0.,
            stage_fraction: 0.,
            current: 0,
            total: 0,
            message: "",
        }),
        level: 0,
        module: Text::new(""),
        message: Text::new(""),
        island: 0,
        src: empty_path(),
        dst: empty_path(),
        layout: Text::new(""),
        bitrate: 0,
        drc_music: 0,
        caf: 0,
        has_lufs: 0,
        has_peak: 0,
        lufs: 0.,
        peak_dbtp: 0.,
        layers: ptr::null(),
        layers_len: 0,
        has_window: 0,
        window_start: 0,
        window_count: 0,
        has_meter_window: 0,
        meter_start: 0,
        meter_count: 0,
        has_matrix: 0,
        matrix: DirectMatrix::empty_view(),
        encoder: Text::new(""),
        date_utc: Text::new(""),
        renderer: Text::new(""),
    }
}

/// The texts and matrix an island record borrows besides the island itself.
#[derive(Default)]
struct IslandStorage {
    texts: Vec<Text>,
    matrix: Option<Box<DirectMatrix>>,
}

/// Fills `e` with `island`, borrowing it and `storage`.
fn island_fields(island: &Island, storage: &mut IslandStorage, e: &mut EngineRenderEvent) {
    e.kind = 3;
    let apple = |e: &mut EngineRenderEvent, plan: &render::ApplePlan| {
        e.dst = path_record(&plan.output_path);
        e.layout = Text::new(&plan.output_layout);
        if let Some(w) = plan.window {
            e.has_window = 1;
            e.window_start = w.start;
            e.window_count = w.count;
        }
        if let Some(w) = plan.meter_window {
            e.has_meter_window = 1;
            e.meter_start = w.start;
            e.meter_count = w.count;
        }
    };
    match island {
        Island::ProbeMp4Packager => e.island = 1,
        Island::ApplePrepare(plan) | Island::AppleRender(plan) => {
            e.island = if matches!(island, Island::ApplePrepare(_)) {
                2
            } else {
                3
            };
            apple(e, plan);
            if let Some(m) = &plan.matrix {
                let matrix = DirectMatrix::new(m.clone());
                e.has_matrix = 1;
                e.matrix = matrix.view();
                storage.matrix = Some(matrix);
            }
        }
        Island::EncodeFlac { src, dst } => {
            e.island = 4;
            e.src = path_record(src);
            e.dst = path_record(dst);
        }
        Island::EncodeOpus {
            src,
            dst,
            layout,
            bitrate,
        } => {
            e.island = 5;
            e.src = path_record(src);
            e.dst = path_record(dst);
            e.layout = Text::new(layout);
            e.bitrate = *bitrate;
        }
        Island::EncodeApac {
            src,
            dst,
            layout,
            bitrate,
            drc_music,
            caf,
        } => {
            e.island = 6;
            e.src = path_record(src);
            e.dst = path_record(dst);
            e.layout = Text::new(layout);
            e.bitrate = *bitrate;
            e.drc_music = u32::from(*drc_music);
            e.caf = u32::from(*caf);
        }
        Island::EncodeIamf {
            src,
            dst,
            layout,
            bitrate,
            lufs,
            peak_dbtp,
            layers,
        } => {
            e.island = 7;
            e.src = path_record(src);
            e.dst = path_record(dst);
            e.layout = Text::new(layout);
            e.bitrate = *bitrate;
            e.has_lufs = u32::from(lufs.is_some());
            e.lufs = lufs.unwrap_or(0.);
            e.has_peak = u32::from(peak_dbtp.is_some());
            e.peak_dbtp = peak_dbtp.unwrap_or(0.);
            storage.texts = layers.iter().map(|l| Text::new(l)).collect();
            e.layers = storage.texts.as_ptr();
            e.layers_len = storage.texts.len();
        }
        Island::PackageMp4 { src, dst } => {
            e.island = 8;
            e.src = path_record(src);
            e.dst = path_record(dst);
        }
        Island::WriteMetadata { path, fields } => {
            e.island = 9;
            e.dst = path_record(path);
            e.encoder = Text::new(&fields.encoder);
            e.date_utc = Text::new(&fields.date_utc);
            e.renderer = Text::new(&fields.renderer);
            e.layout = Text::new(&fields.output_layout);
            e.has_lufs = u32::from(fields.lufs.is_some());
            e.lufs = fields.lufs.unwrap_or(0.);
            e.has_peak = u32::from(fields.peak_dbtp.is_some());
            e.peak_dbtp = fields.peak_dbtp.unwrap_or(0.);
        }
    }
}

/// An island's reply, copying its texts.
pub(crate) unsafe fn reply_of(r: &EngineIslandReply) -> Reply {
    let error = (r.code != 0).then(|| {
        request::Error::new(
            r.code,
            unsafe { string(&r.message) }.unwrap_or_default(),
            unsafe { string(&r.context) }.unwrap_or_default(),
        )
    });
    Reply {
        error,
        metrics: Metrics {
            lufs: (r.has_lufs != 0).then_some(r.lufs),
            peak_dbtp: (r.has_peak != 0).then_some(r.peak_dbtp),
        },
        packager: match r.packager {
            1 => Packager::Mp4box,
            2 => Packager::Ffmpeg,
            _ => Packager::None,
        },
        ffmpeg_major: r.ffmpeg_major,
    }
}

/// The caller's side of an island run inside `mradm_engine_render_next`: `cancelled` polls its
/// stop token (possibly from another thread), `progress` receives an event mapped into the
/// render's range.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IslandContext {
    pub user: *mut c_void,
    pub cancelled: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    pub progress: Option<unsafe extern "C" fn(*mut c_void, *const EngineProgress)>,
    /// A line the island writes (level: `adm::LogLevel`).
    pub log: Option<unsafe extern "C" fn(*mut c_void, u32, Text, Text)>,
}

/// A library's islands: `run` returns 0 when it ran the island (any failure in the reply) and
/// nonzero to leave it to the caller as an island event; `apple` (may be null) runs the Apple
/// backend's islands.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IslandRunner {
    pub run: Option<
        unsafe extern "C" fn(
            *const EngineRenderEvent,
            *const IslandContext,
            *mut EngineIslandReply,
        ) -> i32,
    >,
    pub driver: IslandContext,
    pub apple: *const AppleIslands,
}

/// An island's context while it runs: the caller's context and the render's progress mapping.
struct Bridge<'a> {
    driver: IslandContext,
    map: &'a dyn Fn(&Progress) -> Progress,
}

unsafe extern "C" fn bridge_cancelled(user: *mut c_void) -> u32 {
    let b = unsafe { &*user.cast::<Bridge<'_>>() };
    b.driver
        .cancelled
        .map_or(0, |f| unsafe { f(b.driver.user) })
}

unsafe extern "C" fn bridge_progress(user: *mut c_void, event: *const EngineProgress) {
    let b = unsafe { &*user.cast::<Bridge<'_>>() };
    let (Some(f), Some(e)) = (b.driver.progress, unsafe { event.as_ref() }) else {
        return;
    };
    let mapped = mapped_progress(e, b.map);
    unsafe { f(b.driver.user, &mapped) };
}

unsafe extern "C" fn bridge_log(user: *mut c_void, level: u32, module: Text, message: Text) {
    let b = unsafe { &*user.cast::<Bridge<'_>>() };
    if let Some(f) = b.driver.log {
        unsafe { f(b.driver.user, level, module, message) };
    }
}

impl Bridge<'_> {
    fn context(&self) -> IslandContext {
        IslandContext {
            user: ptr::from_ref(self).cast_mut().cast(),
            cancelled: Some(bridge_cancelled),
            progress: Some(bridge_progress),
            log: Some(bridge_log),
        }
    }
}

/// A reply before the island writes it.
pub(crate) fn empty_reply() -> EngineIslandReply {
    EngineIslandReply {
        code: 0,
        message: Text::new(""),
        context: Text::new(""),
        has_lufs: 0,
        has_peak: 0,
        lufs: 0.,
        peak_dbtp: 0.,
        packager: 0,
        ffmpeg_major: -1,
    }
}

/// The registered islands of one step: the audio runner and the Apple table, with the render's
/// Apple preparation.
struct Runner<'a> {
    islands: &'a IslandRunner,
    apple: Option<Apple>,
    prepared: &'a RefCell<Option<Arc<Preparation>>>,
    made: &'a Cell<bool>,
}

impl Runner<'_> {
    fn apple(
        &self,
        apple: Apple,
        island: &Island,
        scene: Option<&RenderScene>,
        bridge: &Bridge<'_>,
    ) -> Reply {
        let (Island::ApplePrepare(plan) | Island::AppleRender(plan)) = island else {
            unreachable!("an Apple island");
        };
        let settings = Settings::new(
            &plan.input_path,
            &plan.output_layout,
            plan.matrix.as_ref(),
            &plan.options,
        );
        let context = bridge.context();
        let mut reply = empty_reply();
        if matches!(island, Island::ApplePrepare(_)) {
            if let Some(p) = apple.prepare(&settings, scene, &context, &mut reply) {
                *self.prepared.borrow_mut() = Some(p);
                self.made.set(true);
            }
        } else if let Some(p) = self.prepared.borrow().clone() {
            apple.render(
                &p,
                &settings,
                &plan.output_path.utf8,
                plan.window,
                &context,
                &mut reply,
            );
        } else {
            return Reply {
                error: Some(request::Error::new(
                    6,
                    "the Apple preparation is missing",
                    "",
                )),
                ..Reply::default()
            };
        }
        unsafe { reply_of(&reply) }
    }
}

impl render::Islands for Runner<'_> {
    fn run(
        &self,
        island: &Island,
        scene: Option<&RenderScene>,
        map: &dyn Fn(&Progress) -> Progress,
    ) -> Option<Reply> {
        let bridge = Bridge {
            driver: self.islands.driver,
            map,
        };
        if matches!(island, Island::ApplePrepare(_) | Island::AppleRender(_)) {
            return self
                .apple
                .map(|apple| self.apple(apple, island, scene, &bridge));
        }
        if let Some(reply) = own_island(island, &bridge) {
            return Some(reply);
        }
        let run = self.islands.run?;
        let mut storage = IslandStorage::default();
        let mut e = empty_event();
        island_fields(island, &mut storage, &mut e);
        let context = bridge.context();
        let mut reply = empty_reply();
        if unsafe { run(&e, &context, &mut reply) } != 0 {
            return None;
        }
        Some(unsafe { reply_of(&reply) })
    }
}

/// The islands implemented in Rust (PCM consistency 7.2): FLAC encoding and its Vorbis comment.
/// Their progress and cancellation go through the same bridge as the C++ islands'.
fn own_island(island: &Island, bridge: &Bridge<'_>) -> Option<Reply> {
    let user: *mut c_void = ptr::from_ref(bridge).cast_mut().cast();
    let cancelled = || unsafe { bridge_cancelled(user) } != 0;
    let progress = |p: &Progress| {
        let raw = progress_record(p);
        unsafe { bridge_progress(user, &raw) };
    };
    let result = match island {
        Island::EncodeFlac { src, dst } => {
            crate::flac::encode(&src.utf8, &dst.utf8, &cancelled, &progress)
        }
        Island::WriteMetadata { path, fields } if crate::flac::is_flac(&path.utf8) => {
            crate::flac::write_metadata(&path.utf8, fields)
        }
        _ => return None,
    };
    Some(Reply {
        error: result.err(),
        ..Reply::default()
    })
}

/// `e` mapped by `map`; the message borrows `e`'s unless it was empty.
fn mapped_progress(e: &EngineProgress, map: &dyn Fn(&Progress) -> Progress) -> EngineProgress {
    // The range only needs to know whether the message is empty: map with a static stand-in and
    // keep the caller's text.
    let raw = Progress {
        stage: e.stage,
        operation: e.operation,
        fraction: e.fraction,
        stage_fraction: e.stage_fraction,
        current: e.current,
        total: e.total,
        message: if e.message.len == 0 { "" } else { "-" },
    };
    let mut out = progress_record(&map(&raw));
    if e.message.len != 0 {
        out.message = e.message;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    mod apple {
        use super::*;
        use crate::render::apple_island::{ApplePrepared, AppleSettings};
        use mradm_engine::render::{AppleOptions, ApplePlan};
        use mradm_render::offline::Window;
        use std::sync::Mutex;

        static CALLS: Mutex<Vec<String>> = Mutex::new(Vec::new());

        fn call(line: String) {
            CALLS.lock().unwrap().push(line);
        }

        fn text(t: &Text) -> String {
            String::from_utf8(unsafe { crate::render::bytes(t) }.unwrap().to_vec()).unwrap()
        }

        unsafe extern "C" fn prepare(
            s: *const AppleSettings,
            scene: *mut SceneHandle,
            context: *const IslandContext,
            out: *mut *mut ApplePrepared,
            reply: *mut EngineIslandReply,
        ) -> i32 {
            let (s, c) = unsafe { (&*s, &*context) };
            call(format!("prepare {}", settings_text(s)));
            if !scene.is_null() {
                drop(unsafe { Box::from_raw(scene) });
            }
            unsafe { (c.log.unwrap())(c.user, 2, Text::new("apple"), Text::new("prepared")) };
            let raw = progress_record(&Progress {
                stage: 2,
                operation: 0,
                fraction: 0.,
                stage_fraction: 0.5,
                current: 1,
                total: 2,
                message: "",
            });
            unsafe { (c.progress.unwrap())(c.user, &raw) };
            if crate::render::apple_island::settings_fields(s).layout == "bad" {
                let r = unsafe { &mut *reply };
                r.code = 4;
                r.message = Text::new("unsupported layout");
                return 0;
            }
            unsafe { *out = Box::into_raw(Box::new(7u32)).cast() };
            0
        }

        unsafe extern "C" fn render(
            p: *const ApplePrepared,
            s: *const AppleSettings,
            output: Text,
            has_window: u32,
            start: u64,
            count: u64,
            _: *const IslandContext,
            reply: *mut EngineIslandReply,
        ) -> i32 {
            let prepared = unsafe { *p.cast::<u32>() };
            call(format!(
                "render {prepared} {} {} {has_window} {start} {count}",
                text(&output),
                settings_text(unsafe { &*s })
            ));
            let r = unsafe { &mut *reply };
            r.has_lufs = 1;
            r.lufs = -20.;
            0
        }

        unsafe extern "C" fn destroy(p: *mut ApplePrepared) {
            call(format!("destroy {}", unsafe { *p.cast::<u32>() }));
            drop(unsafe { Box::from_raw(p.cast::<u32>()) });
        }

        fn settings_text(s: &AppleSettings) -> String {
            let s = crate::render::apple_island::settings_fields(s);
            format!("{s:?}")
        }

        static TABLE: AppleIslands = crate::render::apple_island::test_table(
            Some(prepare),
            Some(render),
            None,
            Some(destroy),
        );

        #[derive(Default)]
        struct Driver {
            logs: Vec<String>,
            fractions: Vec<f64>,
        }

        unsafe extern "C" fn driver_progress(user: *mut c_void, e: *const EngineProgress) {
            let d = unsafe { &mut *user.cast::<Driver>() };
            d.fractions.push(unsafe { (*e).fraction });
        }

        unsafe extern "C" fn driver_log(
            user: *mut c_void,
            level: u32,
            module: Text,
            message: Text,
        ) {
            let d = unsafe { &mut *user.cast::<Driver>() };
            d.logs
                .push(format!("{level} {} {}", text(&module), text(&message)));
        }

        fn plan(layout: &str) -> ApplePlan {
            let path = |p: &str| PathText {
                utf8: p.into(),
                display: p.into(),
            };
            ApplePlan {
                input_path: path("in.wav"),
                output_path: path("out.wav"),
                output_layout: layout.into(),
                window: Some(Window {
                    start: 10,
                    count: 20,
                }),
                meter_window: None,
                matrix: None,
                options: AppleOptions {
                    routing: 2,
                    split_power: true,
                    speaker_spread: 1,
                    binaural_spread: 3,
                    spatial_preset: 4,
                    speaker_rendering_flags: true,
                    orientation: [10., 5., -3.],
                },
            }
        }

        #[test]
        fn the_apple_runner_prepares_renders_and_keeps_the_preparation() {
            let mut driver = Driver::default();
            let runner = IslandRunner {
                run: None,
                driver: IslandContext {
                    user: ptr::from_mut(&mut driver).cast(),
                    cancelled: None,
                    progress: Some(driver_progress),
                    log: Some(driver_log),
                },
                apple: &TABLE,
            };
            let prepared = RefCell::new(None);
            let made = Cell::new(false);
            let r = Runner {
                islands: &runner,
                apple: Apple::new(runner.apple),
                prepared: &prepared,
                made: &made,
            };
            let map = |p: &Progress| Progress {
                fraction: 0.25 + p.stage_fraction,
                ..*p
            };
            let islands: &dyn render::Islands = &r;
            let reply = islands.run(
                &Island::ApplePrepare(Box::new(plan("binaural"))),
                None,
                &map,
            );
            assert!(reply.unwrap().error.is_none());
            assert!(made.get() && prepared.borrow().is_some());
            let reply = islands
                .run(&Island::AppleRender(Box::new(plan("binaural"))), None, &map)
                .unwrap();
            assert_eq!((reply.error, reply.metrics.lufs), (None, Some(-20.)));
            // Other islands without an audio runner are returned.
            assert!(islands.run(&Island::ProbeMp4Packager, None, &map).is_none());
            let failed = islands
                .run(&Island::ApplePrepare(Box::new(plan("bad"))), None, &map)
                .unwrap();
            assert_eq!(
                failed.error.map(|e| (e.code, e.message)),
                Some((4, "unsupported layout".into()))
            );
            prepared.borrow_mut().take();
            let fields = "AppleFields { input: \"in.wav\", layout: \"binaural\", routing: 2, \
                          has_matrix: 0, split_power: 1, speaker_spread: 1, binaural_spread: 3, \
                          spatial_preset: 4, speaker_rendering_flags: 1, orientation: [10.0, 5.0, -3.0] }";
            let bad = fields.replace("\"binaural\"", "\"bad\"");
            assert_eq!(
                *CALLS.lock().unwrap(),
                [
                    format!("prepare {fields}"),
                    format!("render 7 out.wav {fields} 1 10 20"),
                    format!("prepare {bad}"),
                    "destroy 7".to_owned(),
                ]
            );
            assert_eq!(driver.logs, ["2 apple prepared", "2 apple prepared"]);
            assert_eq!(driver.fractions, [0.75, 0.75]);

            let empty = RefCell::new(None);
            let missing = Runner {
                prepared: &empty,
                ..r
            };
            let reply = (&missing as &dyn render::Islands)
                .run(&Island::AppleRender(Box::new(plan("binaural"))), None, &map)
                .unwrap();
            assert_eq!(
                reply.error.unwrap().message,
                "the Apple preparation is missing"
            );
            let none = Runner {
                apple: None,
                ..missing
            };
            assert!(
                (&none as &dyn render::Islands)
                    .run(
                        &Island::ApplePrepare(Box::new(plan("binaural"))),
                        None,
                        &map
                    )
                    .is_none()
            );
        }
    }
}
