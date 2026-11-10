//! The realtime monitor (formerly the C++ `MonitorEngine`, PCM consistency 5.5 A): a worker renders
//! ahead into an SPSC ring that the device callback drains. The worker applies queued overrides,
//! seeks (cancellable, with the device flush deferred while paused), listener orientations and
//! backend switches (a synchronous seek or a background warmup, then a 2048-frame crossfade or, for
//! output-stage streams, a hard cut), loops, and feeds the LUFS meter; the callback applies HpTF and
//! the 10 ms seek transition and publishes levels. Rules, order, constants and texts follow the C++
//! engine. Rust file streams are driven directly; C++ streams (the Apple island, test streams) come
//! in through the same [`Source`] trait from the FFI boundary.
mod native;
mod pull;
mod worker;

pub use native::Native;

use crate::ring::Ring;
use mradm_dsp::hptf::Live;
use mradm_dsp::meter::{Meter, MeterChannel, MeterMode};
use mradm_dsp::monitor::{Crossfade, Output};
use mradm_render::room_runtime::Control as RoomControl;
use mradm_render::routing::{Error, LiveOverride, Override};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU8, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// `adm::ErrorCode`.
pub mod code {
    pub const INVALID_ARGUMENT: i32 = 1;
    pub const RENDER_FAILED: i32 = 4;
    pub const CANCELLED: i32 = 5;
    pub const INTERNAL_ERROR: i32 = 6;
}

/// `adm::LogLevel`.
pub mod level {
    pub const WARNING: u32 = 2;
    pub const ERROR: u32 = 3;
}

/// Worker production granularity and ring depths (frames); the ring holds channels × frames floats.
const CHUNK_FRAMES: u64 = 1024;
const RING_FRAMES: usize = 8192;
/// Push devices drain into a system queue and tolerate latency: a much deeper lead.
const RING_FRAMES_PUSH: usize = 24576;
/// While head tracking drives the orientation, the realtime lead and chunk shrink.
const TRACKING_LOOKAHEAD_FRAMES: usize = 2048;
const TRACKING_CHUNK_FRAMES: u64 = 512;
const TRACKING_ACTIVE: Duration = Duration::from_millis(750);
const IDLE_NAP: Duration = Duration::from_millis(1);
const CROSSFADE_FRAMES: u64 = 2048;
/// The level channels the callback publishes.
pub const LEVEL_CHANNELS: usize = 64;

/// `IRenderStream::validate_overrides`: read-only, may run on the control thread while the worker
/// renders.
pub trait Validate: Send + Sync {
    fn validate(&self, overrides: &Overrides) -> Result<(), Error>;
}

/// The output stage of a stream that applies the listener orientation in the device callback
/// (`render_output`, realtime-safe) while the worker produces its intermediate.
pub trait OutputStage: Send + Sync {
    fn render_output(
        &self,
        intermediate: &[f32],
        output: &mut [f32],
        frames: usize,
        pose: [f32; 3],
    ) -> usize;
    fn loop_render_reset(&self, frame: u64);
    fn render_failed(&self) -> bool;
}

/// A monitor stream (`IRenderStream`); the worker owns it.
pub trait Source: Send {
    fn out_channels(&self) -> usize;
    fn sample_rate(&self) -> u32;
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error>;
    fn seek(&mut self, frame: u64) -> Result<(), Error>;
    fn seek_with_cancel(&mut self, frame: u64, cancel: &Cancel) -> Result<(), Error>;
    fn seek_requires_preroll(&self) -> bool;
    fn validator(&self) -> Arc<dyn Validate>;
    fn set_overrides(&mut self, overrides: &Overrides);
    fn set_orientation(&mut self, pose: [f32; 3]);
    fn room_control(&self) -> Option<RoomControl> {
        None
    }
    /// Some when the stream renders the orientation at the output with a non-empty intermediate.
    fn output_stage(&self) -> Option<Arc<dyn OutputStage>> {
        None
    }
    fn intermediate_channels(&self) -> usize {
        0
    }
    fn produce_intermediate(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        let _ = (output, frames);
        Ok(0)
    }
    fn reposition_source(&mut self, frame: u64) -> Result<(), Error> {
        self.seek(frame)
    }
}

type Hook = Box<dyn Fn() + Send + Sync>;

/// A `std::stop_source`: once requested it stays requested; watchers (C++ stop sources of island
/// streams) are told when it is requested.
#[derive(Default)]
pub struct Cancel {
    requested: AtomicBool,
    hooks: Mutex<Vec<(u64, Hook)>>,
    next: AtomicU64,
}

impl Cancel {
    pub fn new() -> Arc<Self> {
        Arc::new(Self::default())
    }

    pub fn requested(&self) -> bool {
        self.requested.load(Ordering::SeqCst)
    }

    pub fn request(&self) {
        if !self.requested.swap(true, Ordering::SeqCst) {
            for (_, hook) in self.hooks.lock().unwrap_or_else(|e| e.into_inner()).iter() {
                hook();
            }
        }
    }

    /// Runs `hook` when (or, if already, as soon as) the cancel is requested, until the returned
    /// guard drops.
    pub fn watch(&self, hook: Hook) -> Watch<'_> {
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        self.hooks
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .push((id, hook));
        if self.requested()
            && let Some((_, hook)) = self
                .hooks
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .iter()
                .find(|(i, _)| *i == id)
        {
            hook();
        }
        Watch { cancel: self, id }
    }
}

pub struct Watch<'a> {
    cancel: &'a Cancel,
    id: u64,
}

impl Drop for Watch<'_> {
    fn drop(&mut self) {
        self.cancel
            .hooks
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retain(|(i, _)| *i != self.id);
    }
}

/// What the monitor needs from its caller: the log sink and the device it plays on.
pub trait MonitorHost: Send + Sync {
    fn log(&self, level: u32, module: &str, message: &[u8]);
    fn device_flush(&self);
    fn device_pause(&self);
    fn device_resume(&self);
    /// `set_room_worker_active`: the worker's MMCSS registration while a room renders.
    fn room_worker(&self, active: bool) {
        let _ = active;
    }
    /// `wait_room_worker`: the worker's back-off while a room renders.
    fn room_wait(&self, duration: Duration) {
        std::thread::sleep(duration);
    }
}

/// One owned live edit (`LiveObjectOverride`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ObjectOverride {
    pub object_id: Vec<u8>,
    pub speaker_label: Vec<u8>,
    pub mute: bool,
    pub gain_db: f32,
    pub head_locked: Option<bool>,
    pub extent_scale: f32,
    pub extent_width_scale: f32,
    pub extent_height_scale: f32,
    pub extent_depth_scale: f32,
    pub diffuse_scale: f32,
    pub divergence_scale: f32,
}

/// `LiveOverrides`: the snapshot and its revision.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Overrides {
    pub objects: Vec<ObjectOverride>,
    pub revision: u64,
}

impl Overrides {
    pub fn views(&self) -> Vec<LiveOverride<'_>> {
        self.objects
            .iter()
            .map(|o| LiveOverride {
                base: Override {
                    object_id: &o.object_id,
                    speaker_label: &o.speaker_label,
                    mute: o.mute,
                    gain_db: o.gain_db,
                    head_locked: o.head_locked,
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
}

/// `MonitorState`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    Stopped = 0,
    Playing = 1,
    Paused = 2,
}

impl State {
    fn of(value: u8) -> Self {
        match value {
            1 => Self::Playing,
            2 => Self::Paused,
            _ => Self::Stopped,
        }
    }
}

/// `StreamSwitchState`: pending 0, applied 1, rejected 2. The caller may observe each change.
#[derive(Default)]
pub struct Receipt {
    state: AtomicU8,
    notify: Option<Box<dyn Fn(u8) + Send + Sync>>,
}

impl Receipt {
    pub const PENDING: u8 = 0;
    pub const APPLIED: u8 = 1;
    pub const REJECTED: u8 = 2;
    /// A receipt that tells `notify` its final state.
    pub fn observed(notify: Box<dyn Fn(u8) + Send + Sync>) -> Self {
        Self {
            state: AtomicU8::new(Self::PENDING),
            notify: Some(notify),
        }
    }
    pub fn state(&self) -> u8 {
        self.state.load(Ordering::SeqCst)
    }
    fn finish(receipt: &Option<Arc<Receipt>>, state: u8) {
        if let Some(r) = receipt {
            r.state.store(state, Ordering::SeqCst);
            if let Some(notify) = &r.notify {
                notify(state);
            }
        }
    }
}

/// `MonitorStatus`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Status {
    pub state: State,
    pub playhead_frames: u64,
    pub underruns: u64,
    pub buffered_frames: u64,
    pub ring_fill: f32,
    pub ended: bool,
    pub failed: bool,
    pub override_revision: u64,
}

/// `MonitorLevels`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Levels {
    pub channels: u32,
    pub peak: [f32; LEVEL_CHANNELS],
    pub rms: [f32; LEVEL_CHANNELS],
    pub momentary_lufs: f32,
    pub shortterm_lufs: f32,
    pub integrated_lufs: f32,
}

/// The pull's result: the frames taken from the ring; `mark_end` asks the device to mark the end.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Pulled {
    pub frames: usize,
    pub mark_end: bool,
}

/// A background warmup of an incoming stream (`SwitchWarmup`).
struct Warmup {
    cancel: Arc<Cancel>,
    target: Arc<AtomicU64>,
    receipt: Option<Arc<Receipt>>,
    validator: Arc<dyn Validate>,
    handle: Option<JoinHandle<Warmed>>,
}

struct Warmed {
    stream: Box<dyn Source>,
    position: u64,
    result: Result<(), Error>,
}

/// What the control mutex guards.
struct Control {
    seek_pending: bool,
    seek_in_progress: bool,
    seek_cancel: Arc<Cancel>,
    seek_target: u64,
    play_pending_after_seek: bool,
    device_flush_pending: bool,
    overrides_pending: bool,
    pending_overrides: Overrides,
    orientation_pending: bool,
    pending_orientation: [f32; 3],
    warmup: Option<Warmup>,
    pending_stream_ready: bool,
    switch_pending: bool,
    pending_stream: Option<Box<dyn Source>>,
    pending_receipt: Option<Arc<Receipt>>,
    /// The validators of the worker's current and crossfading streams.
    current_validator: Arc<dyn Validate>,
    xfade_validator: Option<Arc<dyn Validate>>,
}

/// The callback's seek-transition history; only the device thread locks it.
struct Callback {
    output: Output,
}

/// Output stage: the stream's renderer and the popped intermediate. The callback locks it only
/// while active; the worker only while the callback is parked.
struct Stage {
    renderer: Option<Arc<dyn OutputStage>>,
    scratch: Vec<f32>,
}

struct Shared {
    control: Mutex<Control>,
    wake: Condvar,
    host: Arc<dyn MonitorHost>,
    realtime: bool,
    channels: usize,
    sample_rate: u32,
    output_stage: bool,
    ring_channels: usize,
    ring: Ring,
    meter_ring: Ring,
    hptf: Live,
    callback: Mutex<Callback>,
    stage: Mutex<Stage>,
    /// Output stage: the consumer's source position (callback, or the worker while parked).
    consumer_src_pos: AtomicU64,
    room_control: Mutex<Option<RoomControl>>,
    clock: Instant,
    state: AtomicU8,
    quit: AtomicBool,
    in_pop: AtomicBool,
    flushing: AtomicBool,
    ended: AtomicBool,
    failed: AtomicBool,
    frames_played: AtomicU64,
    underruns: AtomicU64,
    loop_start: AtomicU64,
    loop_end: AtomicU64,
    seek_generation: AtomicU64,
    snap: [AtomicU32; 3],
    last_orientation_ns: AtomicI64,
    applied_override_revision: AtomicU64,
    peak: [AtomicU32; LEVEL_CHANNELS],
    rms: [AtomicU32; LEVEL_CHANNELS],
    momentary: AtomicU32,
    shortterm: AtomicU32,
    integrated: AtomicU32,
}

fn store_f32(cell: &AtomicU32, value: f32) {
    cell.store(value.to_bits(), Ordering::Relaxed);
}

fn load_f32(cell: &AtomicU32) -> f32 {
    f32::from_bits(cell.load(Ordering::Relaxed))
}

const SILENCE: f32 = f32::NEG_INFINITY;

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Control> {
        self.control.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn state(&self) -> State {
        State::of(self.state.load(Ordering::SeqCst))
    }

    fn set_state(&self, state: State) {
        self.state.store(state as u8, Ordering::SeqCst);
    }

    fn log(&self, level: u32, module: &str, message: &[u8]) {
        self.host.log(level, module, message);
    }

    /// Nanoseconds since creation, never 0 (0 means no orientation update yet).
    fn now(&self) -> i64 {
        i64::try_from(self.clock.elapsed().as_nanos()).unwrap_or(i64::MAX - 1) + 1
    }

    fn head_tracking_recent(&self) -> bool {
        let last = self.last_orientation_ns.load(Ordering::Relaxed);
        last != 0 && self.now().saturating_sub(last) < TRACKING_ACTIVE.as_nanos() as i64
    }

    fn orientation_snapshot(&self) -> [f32; 3] {
        [
            load_f32(&self.snap[0]),
            load_f32(&self.snap[1]),
            load_f32(&self.snap[2]),
        ]
    }

    fn silence_lufs(&self) {
        store_f32(&self.momentary, SILENCE);
        store_f32(&self.shortterm, SILENCE);
        store_f32(&self.integrated, SILENCE);
    }

    /// Parks the callback: it either sees `flushing` and skips the ring, or it entered first and
    /// the worker waits for it to leave.
    fn park(&self) {
        self.flushing.store(true, Ordering::SeqCst);
        while self.in_pop.load(Ordering::SeqCst) {
            std::thread::yield_now();
        }
    }

    fn unpark(&self) {
        self.flushing.store(false, Ordering::SeqCst);
    }

    fn stage(&self) -> MutexGuard<'_, Stage> {
        self.stage.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// The monitor; the C++ shell owns the device and forwards its callback to [`Self::pull`].
pub struct Monitor {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

/// Why [`Monitor::create`] failed: a returned error, or a construction failure the C++ engine
/// threw from its constructor (the DSP or the HpTF processor).
pub enum CreateError {
    Error(Error),
    Construction(String),
}

fn error(code: i32, message: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: Vec::new(),
    }
}

impl Monitor {
    /// `MonitorEngine::create` after the factory: checks the format, prepares the DSP, the HpTF
    /// processor and the meter, and starts the worker. `realtime` is the device's
    /// `pull_is_realtime_playback`.
    pub fn create(
        source: Box<dyn Source>,
        realtime: bool,
        host: Arc<dyn MonitorHost>,
    ) -> Result<Self, CreateError> {
        let channels = source.out_channels();
        let sample_rate = source.sample_rate();
        if channels == 0 || sample_rate == 0 {
            return Err(CreateError::Error(error(
                code::INTERNAL_ERROR,
                "render stream reports an invalid format (0 channels or 0 sample rate)",
            )));
        }
        let stage = source.output_stage();
        let output_stage = stage.is_some();
        let ring_channels = if output_stage {
            source.intermediate_channels()
        } else {
            channels
        };
        let construction = |e: mradm_dsp::Error| CreateError::Construction(e.to_string());
        let output = Output::new(channels, sample_rate, realtime).map_err(construction)?;
        let crossfade = Crossfade::new(channels, CROSSFADE_FRAMES).map_err(construction)?;
        let hptf = Live::new(channels, sample_rate)
            .map_err(|e| CreateError::Construction(format!("Rust DSP: {e}")))?;
        let ring_frames = if realtime {
            RING_FRAMES
        } else {
            RING_FRAMES_PUSH
        };
        let (pull_scratch, meter_scratch) = if output_stage {
            (
                vec![0.; RING_FRAMES * ring_channels],
                vec![0.; CHUNK_FRAMES as usize * channels],
            )
        } else {
            (Vec::new(), Vec::new())
        };
        let shared = Arc::new(Shared {
            control: Mutex::new(Control {
                seek_pending: false,
                seek_in_progress: false,
                seek_cancel: Cancel::new(),
                seek_target: 0,
                play_pending_after_seek: false,
                device_flush_pending: false,
                overrides_pending: false,
                pending_overrides: Overrides::default(),
                orientation_pending: false,
                pending_orientation: [0.; 3],
                warmup: None,
                pending_stream_ready: false,
                switch_pending: false,
                pending_stream: None,
                pending_receipt: None,
                current_validator: source.validator(),
                xfade_validator: None,
            }),
            wake: Condvar::new(),
            host,
            realtime,
            channels,
            sample_rate,
            output_stage,
            ring_channels,
            ring: Ring::new(ring_frames * ring_channels),
            meter_ring: Ring::new(if output_stage {
                RING_FRAMES * channels
            } else {
                0
            }),
            hptf,
            callback: Mutex::new(Callback { output }),
            stage: Mutex::new(Stage {
                renderer: stage,
                scratch: pull_scratch,
            }),
            consumer_src_pos: AtomicU64::new(0),
            room_control: Mutex::new(source.room_control()),
            clock: Instant::now(),
            state: AtomicU8::new(State::Stopped as u8),
            quit: AtomicBool::new(false),
            in_pop: AtomicBool::new(false),
            flushing: AtomicBool::new(false),
            ended: AtomicBool::new(false),
            failed: AtomicBool::new(false),
            frames_played: AtomicU64::new(0),
            underruns: AtomicU64::new(0),
            loop_start: AtomicU64::new(0),
            loop_end: AtomicU64::new(0),
            seek_generation: AtomicU64::new(0),
            snap: [const { AtomicU32::new(0) }; 3],
            last_orientation_ns: AtomicI64::new(0),
            applied_override_revision: AtomicU64::new(0),
            peak: [const { AtomicU32::new(0) }; LEVEL_CHANNELS],
            rms: [const { AtomicU32::new(0) }; LEVEL_CHANNELS],
            momentary: AtomicU32::new(SILENCE.to_bits()),
            shortterm: AtomicU32::new(SILENCE.to_bits()),
            integrated: AtomicU32::new(SILENCE.to_bits()),
        });
        // macOS lazily allocates Mutex backing storage. Initialize both callback locks before
        // the worker or device can run, including the output stage's first active pull.
        drop(shared.callback.lock().unwrap_or_else(|e| e.into_inner()));
        drop(shared.stage());
        let mut state = worker::Worker::new(shared.clone(), source, crossfade, meter_scratch);
        state.rebuild_meter();
        let handle = std::thread::Builder::new()
            .spawn(move || state.run())
            .map_err(|e| CreateError::Construction(e.to_string()))?;
        Ok(Self {
            shared,
            worker: Some(handle),
        })
    }

    pub fn channels(&self) -> usize {
        self.shared.channels
    }

    pub fn sample_rate(&self) -> u32 {
        self.shared.sample_rate
    }

    pub fn output_stage(&self) -> bool {
        self.shared.output_stage
    }

    /// The HpTF processor the callback runs (publish, bypass and snapshots from the control side).
    pub fn hptf(&self) -> &Live {
        &self.shared.hptf
    }

    /// The first half of the C++ destructor, before the device stops: cancels the seek and
    /// rejects pending switches.
    pub fn begin_shutdown(&self) {
        let s = &self.shared;
        s.quit.store(true, Ordering::Release);
        let c = s.lock();
        c.seek_cancel.request();
        Receipt::finish(&c.pending_receipt, Receipt::REJECTED);
        if let Some(w) = &c.warmup {
            w.cancel.request();
            Receipt::finish(&w.receipt, Receipt::REJECTED);
        }
    }

    /// The device stopped: stop and join the worker (and with it any warmup thread).
    pub fn shutdown(&mut self) {
        let s = &self.shared;
        s.quit.store(true, Ordering::Release);
        s.wake.notify_all();
        if let Some(handle) = self.worker.take() {
            let _ = handle.join();
        }
        let warmup = s.lock().warmup.take();
        if let Some(mut w) = warmup
            && let Some(handle) = w.handle.take()
        {
            let _ = handle.join();
        }
    }

    pub fn play(&self) {
        let s = &self.shared;
        let flush_before_play;
        {
            let mut c = s.lock();
            if s.state() != State::Playing && (c.seek_pending || c.seek_in_progress) {
                // Stay paused until the worker has landed the queued seek.
                c.play_pending_after_seek = true;
                s.wake.notify_all();
                return;
            }
            flush_before_play = c.device_flush_pending;
            c.device_flush_pending = false;
        }
        // Discard a deferred device queue while still paused.
        if flush_before_play {
            s.host.device_flush();
        }
        {
            let _c = s.lock();
            s.set_state(State::Playing);
        }
        s.wake.notify_all();
        s.host.device_resume();
    }

    pub fn pause(&self) {
        let s = &self.shared;
        {
            let mut c = s.lock();
            c.play_pending_after_seek = false;
            s.set_state(State::Paused);
        }
        s.wake.notify_all();
        s.host.device_pause();
    }

    pub fn seek(&self, frame: u64) {
        let s = &self.shared;
        {
            let mut c = s.lock();
            c.seek_cancel.request();
            if let Some(w) = &c.warmup {
                w.cancel.request();
            }
            c.seek_pending = true;
            c.seek_target = frame;
        }
        s.wake.notify_all();
    }

    pub fn set_loop(&self, start: u64, end: u64) {
        let s = &self.shared;
        if end <= start {
            s.loop_end.store(0, Ordering::Relaxed);
            s.loop_start.store(0, Ordering::Relaxed);
            return;
        }
        s.loop_start.store(start, Ordering::Relaxed);
        s.loop_end.store(end, Ordering::Relaxed);
    }

    pub fn set_overrides(&self, overrides: Overrides) -> Result<(), Error> {
        // A finite dB value can still overflow the float gain every backend uses.
        if let Some(invalid) = overrides.objects.iter().find(|o| {
            !o.gain_db.is_finite()
                || (!o.mute && !mradm_dsp::stream_dsp::live_gain(o.gain_db).is_finite())
        }) {
            return Err(Error {
                code: code::INVALID_ARGUMENT,
                message: "实时增益必须为有限值,且未静音时转换为线性增益不得溢出".into(),
                context: invalid.object_id.clone(),
            });
        }
        let s = &self.shared;
        {
            let mut c = s.lock();
            c.current_validator.validate(&overrides)?;
            if let Some(v) = &c.xfade_validator {
                v.validate(&overrides)?;
            }
            if let Some(pending) = &c.pending_stream {
                pending.validator().validate(&overrides)?;
            }
            if let Some(w) = &c.warmup {
                w.validator.validate(&overrides)?;
                w.cancel.request();
            }
            c.pending_overrides = overrides;
            c.overrides_pending = true;
            if c.seek_in_progress {
                c.seek_cancel.request();
                c.seek_pending = true;
            }
        }
        s.wake.notify_all();
        Ok(())
    }

    pub fn set_orientation(&self, pose: [f32; 3]) {
        let s = &self.shared;
        s.last_orientation_ns.store(s.now(), Ordering::Relaxed);
        for (cell, value) in s.snap.iter().zip(pose) {
            store_f32(cell, value);
        }
        {
            let mut c = s.lock();
            c.pending_orientation = pose;
            c.orientation_pending = true;
        }
        s.wake.notify_all();
    }

    /// Queues a hot-switch; the receipt reports whether the worker applied it.
    pub fn switch(&self, next: Box<dyn Source>, receipt: Option<Arc<Receipt>>) {
        let s = &self.shared;
        {
            let mut c = s.lock();
            if let Some(w) = &c.warmup {
                w.cancel.request();
            }
            Receipt::finish(&c.pending_receipt, Receipt::REJECTED);
            c.pending_receipt = receipt;
            c.pending_stream_ready = false;
            c.pending_stream = Some(next);
            c.switch_pending = true;
        }
        s.wake.notify_all();
    }

    pub fn room_control(&self) -> Option<RoomControl> {
        self.shared
            .room_control
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }

    pub fn status(&self) -> Status {
        let s = &self.shared;
        let capacity = s.ring.capacity();
        let buffered = s.ring.available_read();
        Status {
            state: s.state(),
            playhead_frames: s.frames_played.load(Ordering::Relaxed),
            underruns: s.underruns.load(Ordering::Relaxed),
            buffered_frames: buffered.checked_div(s.ring_channels).unwrap_or(0) as u64,
            ring_fill: if capacity > 0 {
                buffered as f32 / capacity as f32
            } else {
                0.
            },
            ended: s.ended.load(Ordering::Relaxed),
            // Acquire pairs with the worker's release: a reported failure's log line is written.
            failed: s.failed.load(Ordering::Acquire),
            override_revision: s.applied_override_revision.load(Ordering::Relaxed),
        }
    }

    pub fn levels(&self) -> Levels {
        let s = &self.shared;
        let channels = s.channels.min(LEVEL_CHANNELS);
        let mut levels = Levels {
            channels: channels as u32,
            peak: [0.; LEVEL_CHANNELS],
            rms: [0.; LEVEL_CHANNELS],
            momentary_lufs: load_f32(&s.momentary),
            shortterm_lufs: load_f32(&s.shortterm),
            integrated_lufs: load_f32(&s.integrated),
        };
        for c in 0..channels {
            levels.peak[c] = load_f32(&s.peak[c]);
            levels.rms[c] = load_f32(&s.rms[c]);
        }
        levels
    }

    /// The device callback: never waits or allocates. `output` holds at least
    /// `frames * channels` floats.
    pub fn pull(&self, output: &mut [f32], frames: usize) -> Pulled {
        pull::pull(&self.shared, output, frames)
    }
}

impl Drop for Monitor {
    fn drop(&mut self) {
        if self.worker.is_some() {
            self.begin_shutdown();
            self.shutdown();
        }
    }
}

/// The meter's channel map: L/R for a stereo feed, otherwise the default layout.
fn meter(channels: usize, rate: u32) -> mradm_dsp::Result<Meter> {
    let stereo = [MeterChannel::Left, MeterChannel::Right];
    let map: &[MeterChannel] = if channels == 2 { &stereo } else { &[] };
    Meter::new(channels as u32, rate, MeterMode::Monitor, map)
}
