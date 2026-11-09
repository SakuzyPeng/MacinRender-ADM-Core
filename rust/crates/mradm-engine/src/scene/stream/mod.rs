//! The Scene stream engine (PCM consistency 5.4 B, formerly the C++ `SceneStreamEngine`): it
//! queues the frames a producer submits, renders them on a worker thread through the generation's
//! [`Renderer`](super::renderer::Renderer), and hands the PCM to the player through a wait-free
//! ring that [`Stream::pull`] drains. The epochs, generations, budgets, fences, backend crossfades,
//! semantic policy, preroll and rational timeline, end of stream, diagnostics and `scene/*`
//! checkpoints keep the C++ rules, order and texts.
mod ring;
mod trace;
mod worker;

use super::renderer::{self, Renderer};
use crate::backend::Host;
use crate::request::Selection;
use mradm_dsp::resampler::Resampler;
use mradm_dsp::scene_transition::Transitions;
use mradm_render::live::{self, field};
use mradm_render::room_runtime::Control as RoomControl;
use mradm_render::routing::Error;
use mradm_scene::{error::Context, live_policy, policy};
use ring::Ring;
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU8, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// `adm::ErrorCode` of the engine's own errors.
pub mod code {
    pub const INVALID_ARGUMENT: i32 = 1;
    pub const UNSUPPORTED: i32 = 2;
    pub const RENDER_FAILED: i32 = 4;
    pub const INTERNAL_ERROR: i32 = 6;
}

/// `adm::LogLevel` of a diagnostic.
pub mod level {
    pub const WARNING: u32 = 2;
    pub const ERROR: u32 = 3;
}

/// `live_scene::DiagnosticCode`.
pub mod diagnostic {
    pub const SEMANTIC_DEGRADED: u32 = 1;
    pub const DIRECT_SPEAKER_FALLBACK: u32 = 2;
    pub const MISSING_LFE_OUTPUT: u32 = 3;
    pub const INCOMPLETE_STATE: u32 = 4;
    pub const BACKEND_FAILURE: u32 = 6;
}

/// `live_scene::FrameFlag`.
pub mod frame_flag {
    pub const STATE_COMPLETE: u32 = 1 << 0;
    pub const WARMUP: u32 = 1 << 1;
    pub const DISCONTINUITY: u32 = 1 << 2;
    pub const CONCEALED: u32 = 1 << 3;
    pub const KNOWN: u32 = STATE_COMPLETE | WARMUP | DISCONTINUITY | CONCEALED;
}

/// `ScenePullFlag`.
pub mod pull_flag {
    pub const BUFFERING: u32 = 1 << 0;
    pub const UNDERRUN: u32 = 1 << 1;
    pub const EOS: u32 = 1 << 2;
    pub const FAILED: u32 = 1 << 3;
}

/// `SceneStreamState`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    Idle = 0,
    Buffering = 1,
    Running = 2,
    Draining = 3,
    Ended = 4,
    Failed = 5,
}

impl State {
    fn of(value: u8) -> Self {
        match value {
            1 => Self::Buffering,
            2 => Self::Running,
            3 => Self::Draining,
            4 => Self::Ended,
            5 => Self::Failed,
            _ => Self::Idle,
        }
    }
}

/// `SceneSubmitStatus`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Submit {
    Accepted = 0,
    WouldBlock = 1,
    TimedOut = 2,
    Closed = 3,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Entity {
    pub id: Vec<u8>,
    pub name: Vec<u8>,
}

/// `live_scene::SemanticIdentity`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Identity {
    pub object_id: Vec<u8>,
    pub object_name: Vec<u8>,
    pub track_uid: Vec<u8>,
    pub importance: Option<i32>,
    pub dialogue_id: Option<i32>,
    pub contents: Vec<Entity>,
    pub programmes: Vec<Entity>,
}

/// `live_scene::ElementDescriptor`; `role` is the producer's value (0 object, 1 DirectSpeakers,
/// 2 LFE). Equality is the C++ `descriptor_equal`: every field, the position even without
/// `has_position`.
#[derive(Clone, Debug, PartialEq)]
pub struct Descriptor {
    pub element_id: u64,
    pub role: u32,
    pub speaker_label: Vec<u8>,
    pub has_position: bool,
    pub position: [f32; 3],
    pub flags: u64,
    pub identity: Option<Identity>,
}

impl Descriptor {
    fn live_role(&self) -> live::Role {
        match self.role {
            0 => live::Role::Object,
            2 => live::Role::Lfe,
            _ => live::Role::DirectSpeaker,
        }
    }

    fn default_state(&self) -> live::State {
        live::default_state(self.live_role(), self.has_position.then_some(self.position))
    }
}

/// The C++ object sizes the input byte budget counts (`sizeof` of `live_scene::Frame`,
/// `PcmPlane`, `StateEntry` and `MetadataUpdate`); the C++ shell passes its own. The defaults are
/// the 64-bit libstdc++ / libc++ / MSVC release layouts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sizes {
    pub frame: usize,
    pub plane: usize,
    pub entry: usize,
    pub update: usize,
}

impl Default for Sizes {
    fn default() -> Self {
        Self {
            frame: 104,
            plane: 40,
            entry: 80,
            update: 120,
        }
    }
}

/// `SceneStreamConfig` without the renderer; zero capacities take the defaults. `manual_clock` is
/// the deterministic replay seam: its start time in nanoseconds, then [`Stream::set_clock`].
#[derive(Clone, Copy, Debug, Default)]
pub struct Config {
    pub output_sample_rate: u32,
    pub input_queue_samples: u32,
    pub input_queue_bytes: u64,
    pub output_ring_frames: u32,
    pub startup_watermark_frames: u32,
    pub manual_clock: Option<i64>,
    pub sizes: Sizes,
}

/// One submitted PCM plane: `has_samples` is whether the producer passed samples at all.
#[derive(Clone, Copy, Debug)]
pub struct PlaneInput {
    pub element_id: u64,
    pub sample_count: u32,
    pub stride: u32,
    pub has_signal: bool,
    pub has_samples: bool,
}

/// The producer's PCM planes. `sample` is only called once a plane passed every check: signalled,
/// with samples, a non-zero stride that does not overflow, and `sample_count` equal to the frame
/// duration; `index` is below that duration.
pub trait PlaneInputs {
    fn len(&self) -> usize;
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
    fn plane(&self, plane: usize) -> PlaneInput;
    fn sample(&self, plane: usize, index: usize) -> f32;
}

/// A borrowed strided plane.
#[derive(Clone, Copy, Debug)]
pub struct PlaneView<'a> {
    pub element_id: u64,
    pub sample_count: u32,
    pub stride: u32,
    pub has_signal: bool,
    pub samples: Option<&'a [f32]>,
}

impl PlaneInputs for [PlaneView<'_>] {
    fn len(&self) -> usize {
        <[PlaneView]>::len(self)
    }
    fn plane(&self, plane: usize) -> PlaneInput {
        let p = &self[plane];
        PlaneInput {
            element_id: p.element_id,
            sample_count: p.sample_count,
            stride: p.stride,
            has_signal: p.has_signal,
            has_samples: p.samples.is_some(),
        }
    }
    fn sample(&self, plane: usize, index: usize) -> f32 {
        let p = &self[plane];
        p.samples.unwrap_or(&[])[index * p.stride as usize]
    }
}

/// `SceneFrameView`.
pub struct FrameInput<'a, P: PlaneInputs + ?Sized> {
    pub epoch: u64,
    pub generation: u64,
    pub start: i64,
    pub duration: u32,
    pub flags: u32,
    pub planes: &'a P,
    pub initial: &'a [live::Entry],
    pub updates: &'a [live::Update],
}

/// `ScenePullResult`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Pull {
    pub flags: u32,
    pub epoch: u64,
    pub first_media_frame: u64,
    pub media_frames: u32,
    pub requested_frames: u32,
}

/// `SceneStreamStatus`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Status {
    pub state: State,
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
    pub ended: bool,
    pub failed: bool,
    pub production_complete: bool,
}

/// `live_scene::Diagnostic`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub level: u32,
    pub code: u32,
    pub epoch: u64,
    pub generation: u64,
    pub element: u64,
    pub field: u64,
    pub message: Vec<u8>,
}

fn error(code: i32, message: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: Vec::new(),
    }
}

fn invalid(message: &str) -> Error {
    error(code::INVALID_ARGUMENT, message)
}

/// A semantic policy error as the former 1024-byte FFI buffer returned it: at most 1023 bytes on
/// a character boundary, read back as a C string.
fn policy_error(e: &mradm_scene::Error, context: &[u8]) -> Error {
    let mut len = e.message.len().min(1023);
    while !e.message.is_char_boundary(len) {
        len -= 1;
    }
    let text = &e.message.as_bytes()[..len];
    let text = text.split(|&b| b == 0).next().unwrap_or(&[]);
    Error {
        code: if (1..=6).contains(&e.code) {
            e.code
        } else {
            code::INTERNAL_ERROR
        },
        message: text.to_vec(),
        context: context.to_vec(),
    }
}

struct Generation {
    id: u64,
    elements: Vec<Descriptor>,
    policy: live_policy::Generation,
}

fn policy_generation(elements: &[Descriptor]) -> Result<live_policy::Generation, Error> {
    let text = |b: &[u8]| String::from_utf8_lossy(b).into_owned();
    let entities = |list: &[Entity]| -> Vec<(String, String)> {
        list.iter().map(|e| (text(&e.id), text(&e.name))).collect()
    };
    let records = elements
        .iter()
        .map(|e| live_policy::Element {
            role: match e.role {
                0 => live_policy::Role::Object,
                2 => live_policy::Role::Lfe,
                _ => live_policy::Role::DirectSpeakers,
            },
            speaker_label: text(&e.speaker_label),
            position: e.has_position.then_some(e.position),
            identity: e.identity.as_ref().map(|i| live_policy::SourceIdentity {
                object_id: text(&i.object_id),
                object_name: text(&i.object_name),
                track_uid: text(&i.track_uid),
                importance: i.importance,
                dialogue_id: i.dialogue_id,
                contents: entities(&i.contents),
                programmes: entities(&i.programmes),
            }),
        })
        .collect();
    live_policy::Generation::new(records).map_err(|e| policy_error(&e, b""))
}

struct PendingBackend {
    selection: Selection,
    renderer: Renderer,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum Kind {
    #[default]
    Frame,
    End,
    Fence,
}

#[derive(Clone, Debug, Default)]
struct OwnedPlane {
    element_id: u64,
    has_signal: bool,
    samples: Vec<f32>,
}

/// A metadata update and its stream order.
#[derive(Clone, Copy, Debug)]
struct MetaUpdate {
    update: live::Update,
    order: u64,
}

#[derive(Clone, Debug, Default)]
struct OwnedFrame {
    epoch: u64,
    generation: u64,
    start: i64,
    duration: u32,
    flags: u32,
    pcm: Vec<OwnedPlane>,
    initial: Vec<live::Entry>,
    updates: Vec<MetaUpdate>,
}

#[derive(Default)]
struct WorkItem {
    kind: Kind,
    serial: u64,
    generation: Option<Arc<Generation>>,
    frame: OwnedFrame,
    reserved_samples: u64,
    reserved_bytes: u64,
    fence_id: u64,
}

/// What the queue mutex guards.
#[derive(Default)]
struct Control {
    queue: VecDeque<WorkItem>,
    generations: HashMap<u64, Arc<Generation>>,
    pending_backend: Option<PendingBackend>,
    orientation_pending: bool,
    pending_orientation: [f32; 3],
    policy_pending: bool,
    pending_policy: Option<Arc<policy::Policy>>,
    pending_policy_revision: u64,
    reserved_samples: u64,
    reserved_bytes: u64,
    has_epoch: bool,
    control_epoch: u64,
    control_target: i64,
    expected_sample: i64,
    has_expected_sample: bool,
    producer_closed: bool,
    reset_request: u64,
    reset_target: i64,
    reset_ack: u64,
    issued_fence: u64,
    completed_fence: u64,
    /// Fences up to this id were issued before the latest begin_epoch.
    cancelled_fence: u64,
}

/// The entries one epoch keeps; later ones are counted (as degradations) but not stored.
pub const DIAGNOSTICS_PER_EPOCH: usize = 4096;

/// The current epoch's entries. Configure warnings carry epoch 0 and belong to the epoch in
/// which they are published.
#[derive(Default)]
struct Log {
    entries: Vec<Diagnostic>,
    epoch: u64,
}

#[derive(Default)]
struct Diagnostics {
    log: Mutex<Log>,
    degradations: AtomicU64,
}

impl Diagnostics {
    fn lock(&self) -> MutexGuard<'_, Log> {
        self.log.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn push(&self, d: Diagnostic) {
        if (diagnostic::SEMANTIC_DEGRADED..=diagnostic::INCOMPLETE_STATE).contains(&d.code) {
            self.degradations.fetch_add(1, Ordering::Relaxed);
        }
        let mut log = self.lock();
        // A worker step of the previous epoch can finish after begin_epoch cleared the log.
        let stale = d.epoch != 0 && d.epoch < log.epoch;
        if !stale && log.entries.len() < DIAGNOSTICS_PER_EPOCH {
            log.entries.push(d);
        }
    }

    /// begin_epoch: the log restarts for `epoch`.
    fn restart(&self, epoch: u64) {
        let mut log = self.lock();
        log.entries.clear();
        log.epoch = epoch;
    }

    /// The renderer warnings of one call, as the C++ adapter published them.
    fn renderer(&self, list: &[live::Diagnostic], epoch: u64, generation: u64) {
        for d in list {
            self.push(Diagnostic {
                level: level::WARNING,
                code: d.code,
                epoch,
                generation,
                element: d.element_id,
                field: d.field,
                message: d.message.as_bytes().to_vec(),
            });
        }
    }
}

enum Clock {
    Monotonic(Instant),
    Manual(AtomicI64),
}

const TRACKING_ACTIVE_NS: i64 = 750_000_000;
const TRACKING_LOOKAHEAD_FRAMES: usize = 2048;

struct Shared {
    control: Mutex<Control>,
    cv: Condvar,
    ring: Ring,
    diagnostics: Diagnostics,
    room_control: Mutex<Option<RoomControl>>,
    clock: Clock,
    channels: usize,
    output_rate: u32,
    input_rate: u32,
    queue_samples: u32,
    queue_bytes: u64,
    ring_frames: u32,
    watermark: u32,
    sizes: Sizes,
    quit: AtomicBool,
    reset_serial: AtomicU64,
    reset_gate: AtomicBool,
    active_pulls: AtomicU32,
    state: AtomicU8,
    epoch_status: AtomicU64,
    generation_status: AtomicU64,
    queued_samples: AtomicU64,
    queued_bytes: AtomicU64,
    media_frames_pulled: AtomicU64,
    underruns: AtomicU64,
    applied_policy_revision: AtomicU64,
    output_ready: AtomicBool,
    output_attached: AtomicBool,
    production_done: AtomicBool,
    failed: AtomicBool,
    last_orientation_ns: AtomicI64,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Control> {
        self.control.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn set_state(&self, state: State, order: Ordering) {
        self.state.store(state as u8, order);
    }

    /// Moves to `to` only from one of `from`: readiness, the end marker and EOS never undo a
    /// later state (a reset and a failure store theirs unconditionally).
    fn advance(&self, from: &[State], to: State) {
        let _ = self
            .state
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                from.contains(&State::of(current)).then_some(to as u8)
            });
    }

    /// Nanoseconds on the stream clock; the monotonic clock never reads 0 ("never updated").
    fn now(&self) -> i64 {
        match &self.clock {
            Clock::Monotonic(base) => {
                i64::try_from(base.elapsed().as_nanos()).unwrap_or(i64::MAX - 1) + 1
            }
            Clock::Manual(ns) => ns.load(Ordering::SeqCst),
        }
    }

    fn head_tracking_recent(&self) -> bool {
        let last = self.last_orientation_ns.load(Ordering::Relaxed);
        last != 0 && self.now().wrapping_sub(last) < TRACKING_ACTIVE_NS
    }

    fn output_lookahead(&self) -> usize {
        let capacity = self.ring_frames as usize;
        if self.head_tracking_recent() {
            capacity.min(TRACKING_LOOKAHEAD_FRAMES)
        } else {
            capacity
        }
    }

    fn effective_watermark(&self) -> usize {
        (self.watermark as usize).min(self.output_lookahead())
    }

    fn buffered_frames(&self) -> usize {
        self.ring.available_read() / self.channels
    }

    fn publish_readiness(&self, buffered: usize, lookahead: usize) {
        if buffered >= (self.watermark as usize).min(lookahead) {
            self.output_ready.store(true, Ordering::Release);
            self.advance(&[State::Buffering], State::Running);
        }
    }

    fn reset_or_quit(&self, serial: u64) -> bool {
        self.quit.load(Ordering::Acquire) || self.reset_serial.load(Ordering::Acquire) != serial
    }

    fn room_coordinates(&self) -> bool {
        self.room_control
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .is_some()
    }

    fn set_room_control(&self, renderer: &Renderer) {
        *self.room_control.lock().unwrap_or_else(|e| e.into_inner()) = renderer.control();
    }

    fn add(&self, level: u32, code: u32, epoch: u64, generation: u64, message: Vec<u8>) {
        self.diagnostics.push(Diagnostic {
            level,
            code,
            epoch,
            generation,
            element: 0,
            field: 0,
            message,
        });
    }

    fn enter_failed_state(&self) {
        self.failed.store(true, Ordering::Release);
        self.production_done.store(true, Ordering::Release);
        self.output_ready.store(true, Ordering::Release);
        self.set_state(State::Failed, Ordering::Release);
        {
            let mut c = self.lock();
            c.producer_closed = true;
            c.queue.clear();
            c.reserved_samples = 0;
            c.reserved_bytes = 0;
            self.queued_samples.store(0, Ordering::Relaxed);
            self.queued_bytes.store(0, Ordering::Relaxed);
        }
        self.cv.notify_all();
    }

    fn fail(&self, e: &Error, epoch: u64, generation: u64) {
        let mut message = e.message.clone();
        if !e.context.is_empty() {
            message.extend_from_slice(b": ");
            message.extend_from_slice(&e.context);
        }
        self.add(
            level::ERROR,
            diagnostic::BACKEND_FAILURE,
            epoch,
            generation,
            message,
        );
        self.enter_failed_state();
    }

    fn fail_worker_exception(&self, context: &[u8], epoch: u64, generation: u64) {
        self.fail(
            &Error {
                code: code::INTERNAL_ERROR,
                message: b"live Scene worker threw an exception".to_vec(),
                context: context.to_vec(),
            },
            epoch,
            generation,
        );
    }

    fn store_reserved(&self, c: &Control) {
        self.queued_samples
            .store(c.reserved_samples, Ordering::Relaxed);
        self.queued_bytes.store(c.reserved_bytes, Ordering::Relaxed);
    }

    fn release_budget(&self, item: &WorkItem) {
        let mut c = self.lock();
        if item.serial != self.reset_serial.load(Ordering::Relaxed) {
            return;
        }
        c.reserved_samples -= c.reserved_samples.min(item.reserved_samples);
        c.reserved_bytes -= c.reserved_bytes.min(item.reserved_bytes);
        self.store_reserved(&c);
        drop(c);
        self.cv.notify_all();
    }
}

/// `condition_variable::wait_for(lock, timeout, predicate)`: true when the predicate holds; a
/// negative timeout has already expired and one past the clock's range never does.
fn wait_for<'a>(
    shared: &'a Shared,
    mut guard: MutexGuard<'a, Control>,
    timeout_ms: i64,
    mut ready: impl FnMut(&Control) -> bool,
) -> (MutexGuard<'a, Control>, bool) {
    let deadline =
        Instant::now().checked_add(Duration::from_millis(timeout_ms.max(0).unsigned_abs()));
    loop {
        if ready(&guard) {
            return (guard, true);
        }
        match deadline {
            None => guard = shared.cv.wait(guard).unwrap_or_else(|e| e.into_inner()),
            Some(deadline) => {
                let now = Instant::now();
                if now >= deadline {
                    let result = ready(&guard);
                    return (guard, result);
                }
                guard = shared
                    .cv
                    .wait_timeout(guard, deadline - now)
                    .unwrap_or_else(|e| e.into_inner())
                    .0;
            }
        }
    }
}

fn finite(v: f32) -> bool {
    v.is_finite()
}

/// The producer ranges of a submitted state; room streams take room coordinates.
fn finite_state(s: &live::State, room: bool) -> bool {
    let has = |f: u64| s.valid_fields & f != 0;
    let coordinate = |v: f32| finite(v) && (room || (-1. ..=1.).contains(&v));
    let unit = |v: f32| finite(v) && (0. ..=1.).contains(&v);
    if s.valid_fields & !field::KNOWN != 0 {
        return false;
    }
    if has(field::LINEAR_GAIN) && (!finite(s.linear_gain) || s.linear_gain < 0.) {
        return false;
    }
    if has(field::POSITION) && !s.position.iter().all(|&v| coordinate(v)) {
        return false;
    }
    if has(field::EXTENT) && !(unit(s.width) && unit(s.height) && unit(s.depth)) {
        return false;
    }
    if has(field::DIFFUSE) && !unit(s.diffuse) {
        return false;
    }
    if has(field::DIVERGENCE) && !unit(s.divergence) {
        return false;
    }
    if has(field::DIVERGENCE_RANGE)
        && (!finite(s.divergence_azimuth_range)
            || s.divergence_azimuth_range < 0.
            || !finite(s.divergence_position_range)
            || s.divergence_position_range < 0.)
    {
        return false;
    }
    if has(field::CHANNEL_LOCK_MAX_DISTANCE)
        && s.channel_lock_max_distance
            .is_some_and(|d| !finite(d) || d < 0.)
    {
        return false;
    }
    true
}

fn add_size(total: &mut usize, count: usize, item: usize) -> bool {
    if count > (usize::MAX - *total) / item {
        return false;
    }
    *total += count * item;
    true
}

const MIN_RATE: u32 = 8000;
const MAX_RATE: u32 = 192_000;
const DEFAULT_INPUT_SAMPLES: u32 = 32768;
const DEFAULT_INPUT_BYTES: u64 = 64 * 1024 * 1024;
const DEFAULT_OUTPUT_FRAMES: u32 = 8192;
const DEFAULT_WATERMARK_FRAMES: u32 = 4096;
const BACKEND_CROSSFADE_FRAMES: u64 = 2048;

/// A Scene stream; every method may run on any thread, [`Stream::pull`] on the audio thread.
pub struct Stream {
    shared: Arc<Shared>,
    host: Arc<dyn Host + Send + Sync>,
    worker: Option<JoinHandle<()>>,
}

impl Drop for Stream {
    fn drop(&mut self) {
        {
            // The terminal predicate is published under the wait's mutex so an idle worker cannot
            // miss it.
            let _c = self.shared.lock();
            self.shared.reset_gate.store(true, Ordering::Release);
            self.shared.quit.store(true, Ordering::Release);
        }
        self.shared.cv.notify_all();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Stream {
    /// `SceneStreamEngine::create`: `renderer` is the first backend (its `sink` should be set).
    pub fn create(
        config: &Config,
        renderer: &renderer::Config,
        host: Arc<dyn Host + Send + Sync>,
    ) -> Result<Self, Error> {
        let input_rate = renderer.sample_rate;
        if !(MIN_RATE..=MAX_RATE).contains(&input_rate)
            || !(MIN_RATE..=MAX_RATE).contains(&config.output_sample_rate)
        {
            return Err(invalid(
                "Scene stream sample rates must be in the 8-192 kHz range",
            ));
        }
        let or = |v: u32, d: u32| if v == 0 { d } else { v };
        let queue_samples = or(config.input_queue_samples, DEFAULT_INPUT_SAMPLES);
        let queue_bytes = if config.input_queue_bytes == 0 {
            DEFAULT_INPUT_BYTES
        } else {
            config.input_queue_bytes
        };
        let ring_frames = or(config.output_ring_frames, DEFAULT_OUTPUT_FRAMES);
        let watermark = or(config.startup_watermark_frames, DEFAULT_WATERMARK_FRAMES);
        if watermark > ring_frames {
            return Err(invalid("Scene stream queue/ring capacities are invalid"));
        }
        let created = renderer::create(renderer, host.as_ref())?;
        let channels = created.renderer.output_channels() as usize;
        if channels == 0 {
            return Err(error(
                code::INTERNAL_ERROR,
                "live Scene renderer returned an invalid output format",
            ));
        }
        if channels > usize::MAX / ring_frames as usize {
            return Err(invalid(
                "Scene output ring size overflows addressable memory",
            ));
        }
        let failed_create = |detail: String| {
            error(
                code::INTERNAL_ERROR,
                format!("failed to create live Scene stream: {detail}"),
            )
        };
        let transitions = Transitions::new(
            channels,
            config.output_sample_rate,
            BACKEND_CROSSFADE_FRAMES,
        )
        .map_err(|e| failed_create(e.to_string()))?;
        let resampler = if input_rate != config.output_sample_rate {
            Some(
                Resampler::new(channels, input_rate, config.output_sample_rate)
                    .map_err(|e| failed_create(e.to_string()))?,
            )
        } else {
            None
        };
        let shared = Arc::new(Shared {
            control: Mutex::new(Control {
                producer_closed: true,
                ..Control::default()
            }),
            cv: Condvar::new(),
            ring: Ring::new(channels * ring_frames as usize),
            diagnostics: Diagnostics::default(),
            room_control: Mutex::new(created.renderer.control()),
            clock: match config.manual_clock {
                Some(ns) => Clock::Manual(AtomicI64::new(ns)),
                None => Clock::Monotonic(Instant::now()),
            },
            channels,
            output_rate: config.output_sample_rate,
            input_rate,
            queue_samples,
            queue_bytes,
            ring_frames,
            watermark,
            sizes: config.sizes,
            quit: AtomicBool::new(false),
            reset_serial: AtomicU64::new(0),
            reset_gate: AtomicBool::new(false),
            active_pulls: AtomicU32::new(0),
            state: AtomicU8::new(State::Idle as u8),
            epoch_status: AtomicU64::new(0),
            generation_status: AtomicU64::new(0),
            queued_samples: AtomicU64::new(0),
            queued_bytes: AtomicU64::new(0),
            media_frames_pulled: AtomicU64::new(0),
            underruns: AtomicU64::new(0),
            applied_policy_revision: AtomicU64::new(0),
            output_ready: AtomicBool::new(false),
            output_attached: AtomicBool::new(false),
            production_done: AtomicBool::new(false),
            failed: AtomicBool::new(false),
            last_orientation_ns: AtomicI64::new(0),
        });
        let state = worker::Worker::new(
            shared.clone(),
            host.clone(),
            created.renderer,
            renderer.selection,
            resampler,
            transitions,
        );
        let worker = std::thread::Builder::new()
            .spawn(move || state.run())
            .map_err(|e| failed_create(e.to_string()))?;
        Ok(Self {
            shared,
            host,
            worker: Some(worker),
        })
    }

    /// (sample rate, channels) of the pulled PCM.
    pub fn output_format(&self) -> (u32, u32) {
        (self.shared.output_rate, self.shared.channels as u32)
    }

    pub fn input_sample_rate(&self) -> u32 {
        self.shared.input_rate
    }

    pub fn begin_epoch(&self, epoch: u64, target: i64) -> Result<(), Error> {
        let s = &self.shared;
        let monotonic = |c: &Control| !(c.has_epoch && epoch <= c.control_epoch);
        if !monotonic(&s.lock()) {
            return Err(invalid("Scene epoch IDs must increase monotonically"));
        }
        s.reset_gate.store(true, Ordering::SeqCst);
        while s.active_pulls.load(Ordering::SeqCst) != 0 {
            std::thread::yield_now();
        }
        let mut c = s.lock();
        if !monotonic(&c) {
            s.reset_gate.store(false, Ordering::SeqCst);
            return Err(invalid("Scene epoch IDs must increase monotonically"));
        }
        let serial = s.reset_serial.load(Ordering::Relaxed) + 1;
        s.reset_serial.store(serial, Ordering::Release);
        s.diagnostics.restart(epoch);
        c.cancelled_fence = c.issued_fence;
        c.queue.clear();
        c.generations.clear();
        c.reserved_samples = 0;
        c.reserved_bytes = 0;
        s.queued_samples.store(0, Ordering::Relaxed);
        s.queued_bytes.store(0, Ordering::Relaxed);
        c.has_epoch = true;
        c.control_epoch = epoch;
        c.control_target = target;
        c.expected_sample = target;
        c.has_expected_sample = false;
        c.producer_closed = false;
        c.reset_target = target;
        s.epoch_status.store(epoch, Ordering::Release);
        s.generation_status.store(0, Ordering::Release);
        s.set_state(State::Buffering, Ordering::Release);
        c.reset_request = serial;
        s.cv.notify_all();
        let c =
            s.cv.wait_while(c, |c| {
                !(c.reset_ack >= serial || s.quit.load(Ordering::Acquire))
            })
            .unwrap_or_else(|e| e.into_inner());
        drop(c);
        s.reset_gate.store(false, Ordering::SeqCst);
        if s.quit.load(Ordering::Acquire) {
            return Err(error(
                code::INTERNAL_ERROR,
                "Scene stream closed while resetting its epoch",
            ));
        }
        if s.failed.load(Ordering::Acquire) {
            return Err(error(
                code::INTERNAL_ERROR,
                "Scene stream worker failed while resetting its epoch",
            ));
        }
        Ok(())
    }

    pub fn configure_generation(
        &self,
        epoch: u64,
        generation: u64,
        elements: &[Descriptor],
    ) -> Result<(), Error> {
        let s = &self.shared;
        let room = s.room_coordinates();
        let mut ids = HashSet::with_capacity(elements.len());
        for e in elements {
            if e.role > 2 {
                return Err(invalid("Scene generation contains an invalid element role"));
            }
            if !ids.insert(e.element_id) {
                return Err(invalid("Scene generation contains a duplicate element ID"));
            }
            if e.has_position
                && !e
                    .position
                    .iter()
                    .all(|&v| finite(v) && (room || (-1. ..=1.).contains(&v)))
            {
                return Err(invalid(
                    "Scene element Cartesian position must use finite canonical [-1, 1] coordinates",
                ));
            }
        }
        let model = Arc::new(Generation {
            id: generation,
            elements: elements.to_vec(),
            policy: policy_generation(elements)?,
        });
        let mut c = s.lock();
        if !c.has_epoch || epoch != c.control_epoch {
            return Err(invalid("Scene generation uses an unknown epoch"));
        }
        if c.producer_closed {
            return Err(invalid("Scene epoch is closed"));
        }
        if let Some(found) = c.generations.get(&generation) {
            if found.elements != elements {
                return Err(invalid(
                    "Scene generation topology cannot change after configuration",
                ));
            }
            return Ok(());
        }
        c.generations.insert(generation, model);
        Ok(())
    }

    /// Validation, budget reservation and the deep copy form one transaction: an accepted frame
    /// is owned.
    pub fn submit_frame<P: PlaneInputs + ?Sized>(
        &self,
        view: &FrameInput<'_, P>,
        timeout_ms: i64,
    ) -> Result<Submit, Error> {
        let s = &self.shared;
        let duration = view.duration;
        if duration == 0 || view.start > i64::MAX - i64::from(duration) {
            return Err(invalid("Scene frame duration/timeline is invalid"));
        }
        if view.flags & !frame_flag::KNOWN != 0 {
            return Err(invalid("Scene frame has unknown flags"));
        }
        let generation = {
            let c = s.lock();
            if !c.has_epoch || view.epoch != c.control_epoch {
                return Err(invalid("Scene frame uses an unknown epoch"));
            }
            match c.generations.get(&view.generation) {
                Some(g) => g.clone(),
                None => return Err(invalid("Scene frame uses an unknown generation")),
            }
        };
        if view.planes.len() != generation.elements.len() {
            return Err(invalid(
                "Scene frame must provide one PCM plane per element",
            ));
        }
        let element_ids: HashSet<u64> = generation.elements.iter().map(|e| e.element_id).collect();
        let mut plane_ids = HashSet::with_capacity(view.planes.len());
        let sizes = s.sizes;
        let mut owned = sizes.frame;
        if !add_size(&mut owned, view.planes.len(), sizes.plane)
            || !add_size(&mut owned, view.initial.len(), sizes.entry)
            || !add_size(&mut owned, view.updates.len(), sizes.update)
        {
            return Err(invalid(
                "Scene frame owned size overflows addressable memory",
            ));
        }
        for index in 0..view.planes.len() {
            let p = view.planes.plane(index);
            if !plane_ids.insert(p.element_id) || !element_ids.contains(&p.element_id) {
                return Err(invalid(
                    "Scene PCM plane has a duplicate or unknown element ID",
                ));
            }
            if p.sample_count != duration {
                return Err(invalid(
                    "Scene PCM plane sample count must equal frame duration",
                ));
            }
            if p.has_signal && (!p.has_samples || p.stride == 0) {
                return Err(invalid("signalled Scene PCM plane has no usable samples"));
            }
            if p.has_signal
                && duration > 1
                && u64::from(p.stride) * u64::from(duration - 1)
                    > (usize::MAX / std::mem::size_of::<f32>()) as u64
            {
                return Err(invalid("Scene PCM stride overflows addressable memory"));
            }
            if !add_size(
                &mut owned,
                if p.has_signal { duration as usize } else { 0 },
                std::mem::size_of::<f32>(),
            ) {
                return Err(invalid("Scene PCM owned size overflows addressable memory"));
            }
            if p.has_signal
                && (0..duration as usize).any(|k| !view.planes.sample(index, k).is_finite())
            {
                return Err(invalid("Scene PCM samples must be finite"));
            }
        }
        let room = s.room_coordinates();
        let mut initial_ids = HashSet::new();
        if view.initial.iter().any(|i| {
            !element_ids.contains(&i.element_id)
                || !initial_ids.insert(i.element_id)
                || !finite_state(&i.state, room)
        }) {
            return Err(invalid(
                "Scene initial state has an unknown/duplicate element or invalid value",
            ));
        }
        if view.updates.iter().any(|u| {
            !element_ids.contains(&u.element_id)
                || u.offset >= duration
                || u.changed == 0
                || u.changed & !field::KNOWN != 0
                || u.changed & !u.state.valid_fields != 0
                || !finite_state(&u.state, room)
        }) {
            return Err(invalid("Scene metadata update is invalid"));
        }
        if duration > s.queue_samples || owned as u64 > s.queue_bytes {
            return Err(invalid(
                "Scene frame exceeds the configured input queue budget",
            ));
        }
        let owned = owned as u64;

        let serial;
        {
            let c = s.lock();
            if view.flags & frame_flag::DISCONTINUITY != 0 && c.has_expected_sample {
                return Err(invalid(
                    "Scene discontinuity flag is only valid on the first frame after begin_epoch",
                ));
            }
            let timeline_valid = if c.has_expected_sample {
                view.start == c.expected_sample
            } else {
                view.start <= c.control_target
            };
            if !timeline_valid {
                return Err(invalid("Scene frame creates a timeline gap or overlap"));
            }
            let capacity = |c: &Control| {
                c.reserved_samples + u64::from(duration) <= u64::from(s.queue_samples)
                    && c.reserved_bytes + owned <= s.queue_bytes
            };
            let closed = |c: &Control| {
                c.producer_closed
                    || s.failed.load(Ordering::Acquire)
                    || s.quit.load(Ordering::Acquire)
                    || view.epoch != c.control_epoch
            };
            if closed(&c) {
                return Ok(Submit::Closed);
            }
            let mut c = c;
            if !capacity(&c) {
                if timeout_ms == 0 {
                    return Ok(Submit::WouldBlock);
                }
                let (guard, awakened) = wait_for(s, c, timeout_ms, |c| capacity(c) || closed(c));
                c = guard;
                if !awakened {
                    return Ok(Submit::TimedOut);
                }
                if closed(&c) {
                    return Ok(Submit::Closed);
                }
            }
            serial = s.reset_serial.load(Ordering::Relaxed);
            c.reserved_samples += u64::from(duration);
            c.reserved_bytes += owned;
            s.store_reserved(&c);
        }

        let mut frame = OwnedFrame {
            epoch: view.epoch,
            generation: view.generation,
            start: view.start,
            duration,
            flags: view.flags,
            pcm: Vec::with_capacity(view.planes.len()),
            initial: view.initial.to_vec(),
            updates: view
                .updates
                .iter()
                .enumerate()
                .map(|(order, &update)| MetaUpdate {
                    update,
                    order: order as u64,
                })
                .collect(),
        };
        for index in 0..view.planes.len() {
            let p = view.planes.plane(index);
            frame.pcm.push(OwnedPlane {
                element_id: p.element_id,
                has_signal: p.has_signal,
                samples: if p.has_signal {
                    (0..duration as usize)
                        .map(|k| view.planes.sample(index, k))
                        .collect()
                } else {
                    Vec::new()
                },
            });
        }
        frame.updates.sort_by_key(|u| u.update.offset);
        let item = WorkItem {
            kind: Kind::Frame,
            serial,
            generation: Some(generation),
            frame,
            reserved_samples: u64::from(duration),
            reserved_bytes: owned,
            fence_id: 0,
        };
        {
            let mut c = s.lock();
            if c.producer_closed || serial != s.reset_serial.load(Ordering::Relaxed) {
                c.reserved_samples -= c.reserved_samples.min(item.reserved_samples);
                c.reserved_bytes -= c.reserved_bytes.min(item.reserved_bytes);
                s.store_reserved(&c);
                return Ok(Submit::Closed);
            }
            c.queue.push_back(item);
            c.expected_sample = view.start + i64::from(duration);
            c.has_expected_sample = true;
        }
        s.cv.notify_one();
        Ok(Submit::Accepted)
    }

    pub fn signal_end(&self, epoch: u64, end: i64) -> Result<(), Error> {
        let s = &self.shared;
        let mut c = s.lock();
        if !c.has_epoch || epoch != c.control_epoch {
            return Err(invalid("Scene end marker uses an unknown epoch"));
        }
        if c.producer_closed {
            return Err(invalid("Scene epoch is already closed"));
        }
        let expected = if c.has_expected_sample {
            c.expected_sample
        } else {
            c.control_target
        };
        if end != expected {
            return Err(invalid(
                "Scene end marker does not match the next expected sample",
            ));
        }
        c.queue.push_back(WorkItem {
            kind: Kind::End,
            serial: s.reset_serial.load(Ordering::Relaxed),
            // The tail's slices continue the timeline at the end sample.
            frame: OwnedFrame {
                epoch,
                start: end,
                ..OwnedFrame::default()
            },
            ..WorkItem::default()
        });
        c.producer_closed = true;
        s.advance(&[State::Buffering, State::Running], State::Draining);
        drop(c);
        s.cv.notify_one();
        Ok(())
    }

    /// Prepares the next backend on the caller's thread; the worker crossfades to it.
    pub fn switch_backend(&self, config: &renderer::Config) -> Result<(), Error> {
        let s = &self.shared;
        if config.sample_rate != s.input_rate {
            return Err(error(
                code::UNSUPPORTED,
                "live Scene backend switches cannot change the input sample rate",
            ));
        }
        let created = renderer::create(config, self.host.as_ref())?;
        if created.renderer.output_channels() == 0 {
            return Err(error(
                code::INTERNAL_ERROR,
                "prepared live Scene renderer has an invalid output format",
            ));
        }
        if created.sample_rate != s.input_rate {
            return Err(error(
                code::UNSUPPORTED,
                "live Scene backend switches cannot change the renderer sample rate",
            ));
        }
        if created.renderer.output_channels() as usize != s.channels {
            return Err(error(
                code::UNSUPPORTED,
                "live Scene backend switches cannot change the output channel count",
            ));
        }
        s.lock().pending_backend = Some(PendingBackend {
            selection: config.selection,
            renderer: created.renderer,
        });
        s.cv.notify_one();
        Ok(())
    }

    /// Fences the submitted work and controls; a replay consumer drains the ring first.
    pub fn wait_idle(&self, timeout_ms: i64) -> Result<(), Error> {
        let s = &self.shared;
        let mut c = s.lock();
        c.issued_fence += 1;
        let id = c.issued_fence;
        c.queue.push_back(WorkItem {
            kind: Kind::Fence,
            serial: s.reset_serial.load(Ordering::SeqCst),
            fence_id: id,
            ..WorkItem::default()
        });
        s.cv.notify_all();
        let (c, done) = wait_for(s, c, timeout_ms, |c| {
            c.completed_fence >= id
                || c.cancelled_fence >= id
                || s.failed.load(Ordering::SeqCst)
                || s.quit.load(Ordering::SeqCst)
        });
        if !done {
            return Err(error(code::RENDER_FAILED, "Scene replay fence timed out"));
        }
        // A new epoch discarded the fenced work.
        if c.completed_fence < id && c.cancelled_fence >= id {
            return Err(error(
                code::RENDER_FAILED,
                "Scene replay fence was cancelled by a new epoch",
            ));
        }
        drop(c);
        if s.failed.load(Ordering::SeqCst) || s.quit.load(Ordering::SeqCst) {
            return Err(error(
                code::RENDER_FAILED,
                "Scene replay worker failed or closed",
            ));
        }
        Ok(())
    }

    /// (yaw, pitch, roll) in degrees.
    pub fn set_listener_orientation(&self, pose: [f32; 3]) {
        let s = &self.shared;
        s.last_orientation_ns.store(s.now(), Ordering::Relaxed);
        {
            let mut c = s.lock();
            c.pending_orientation = pose;
            c.orientation_pending = true;
        }
        s.cv.notify_one();
    }

    pub fn room_control(&self) -> Option<RoomControl> {
        self.shared
            .room_control
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }

    /// An empty document clears the policy; the worker applies it at the next slice.
    pub fn set_semantic_policy_json(&self, json: &[u8], revision: u64) -> Result<(), Error> {
        let parsed = if json.is_empty() {
            None
        } else {
            Some(Arc::new(policy::parse(json).map_err(|e| {
                let context: &[u8] = if e.context == Context::Label {
                    b"<scene-stream>"
                } else {
                    b""
                };
                policy_error(&e, context)
            })?))
        };
        let s = &self.shared;
        {
            let mut c = s.lock();
            c.pending_policy = parsed;
            c.pending_policy_revision = revision;
            c.policy_pending = true;
        }
        s.cv.notify_one();
        Ok(())
    }

    /// The audio thread's read: never blocks or allocates. `output` holds at least
    /// `frames * channels` samples; they are zeroed first.
    pub fn pull(&self, output: &mut [f32], frames: u32) -> Pull {
        let s = &self.shared;
        let mut result = Pull {
            requested_frames: frames,
            epoch: s.epoch_status.load(Ordering::Acquire),
            first_media_frame: s.media_frames_pulled.load(Ordering::Relaxed),
            ..Pull::default()
        };
        if frames == 0 {
            return result;
        }
        let output = &mut output[..frames as usize * s.channels];
        output.fill(0.);
        if s.reset_gate.load(Ordering::SeqCst) {
            result.flags |= pull_flag::BUFFERING;
            return result;
        }
        s.active_pulls.fetch_add(1, Ordering::SeqCst);
        if s.reset_gate.load(Ordering::SeqCst) {
            s.active_pulls.fetch_sub(1, Ordering::SeqCst);
            result.flags |= pull_flag::BUFFERING;
            return result;
        }
        let available = s.buffered_frames();
        let done = s.production_done.load(Ordering::Acquire);
        let mut ready = s.output_ready.load(Ordering::Acquire);
        if !ready && (available >= s.effective_watermark() || done) {
            s.output_ready.store(true, Ordering::Release);
            ready = true;
        }
        if !ready {
            result.flags |= pull_flag::BUFFERING;
        } else {
            let popped = s.ring.pop(output);
            result.media_frames = (popped / s.channels) as u32;
            if result.media_frames > 0 {
                s.media_frames_pulled
                    .fetch_add(u64::from(result.media_frames), Ordering::Relaxed);
            }
            if result.media_frames < frames {
                if done && s.ring.available_read() == 0 && !s.failed.load(Ordering::Acquire) {
                    result.flags |= pull_flag::EOS;
                    s.advance(
                        &[State::Buffering, State::Running, State::Draining],
                        State::Ended,
                    );
                } else if !s.failed.load(Ordering::Acquire) {
                    result.flags |= pull_flag::UNDERRUN;
                    s.underruns.fetch_add(1, Ordering::Relaxed);
                }
            }
        }
        if s.failed.load(Ordering::Acquire) {
            result.flags |= pull_flag::FAILED;
        }
        s.active_pulls.fetch_sub(1, Ordering::SeqCst);
        result
    }

    pub fn attach_output(&self) -> bool {
        self.shared
            .output_attached
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_ok()
    }

    pub fn detach_output(&self) {
        self.shared.output_attached.store(false, Ordering::SeqCst);
    }

    pub fn output_attached(&self) -> bool {
        self.shared.output_attached.load(Ordering::SeqCst)
    }

    pub fn status(&self) -> Status {
        let s = &self.shared;
        let state = State::of(s.state.load(Ordering::Acquire));
        let buffered = s.buffered_frames() as u64;
        Status {
            state,
            epoch: s.epoch_status.load(Ordering::Acquire),
            generation: s.generation_status.load(Ordering::Acquire),
            queued_input_samples: s.queued_samples.load(Ordering::Relaxed),
            queued_input_bytes: s.queued_bytes.load(Ordering::Relaxed),
            buffered_output_frames: buffered,
            media_frames_pulled: s.media_frames_pulled.load(Ordering::Relaxed),
            underruns: s.underruns.load(Ordering::Relaxed),
            semantic_degradations: s.diagnostics.degradations.load(Ordering::Relaxed),
            semantic_policy_revision: s.applied_policy_revision.load(Ordering::Acquire),
            ring_fill: buffered as f32 / s.ring_frames as f32,
            ended: state == State::Ended,
            failed: s.failed.load(Ordering::Acquire),
            production_complete: s.production_done.load(Ordering::Acquire),
        }
    }

    /// The entries of the current epoch (at most [`DIAGNOSTICS_PER_EPOCH`]).
    pub fn diagnostic_count(&self) -> usize {
        self.shared.diagnostics.lock().entries.len()
    }

    pub fn diagnostic(&self, index: usize) -> Option<Diagnostic> {
        self.shared.diagnostics.lock().entries.get(index).cloned()
    }

    /// The manual replay clock's time in nanoseconds; ignored on the monotonic clock.
    pub fn set_clock(&self, ns: i64) {
        if let Clock::Manual(clock) = &self.shared.clock {
            clock.store(ns, Ordering::SeqCst);
        }
    }
}
