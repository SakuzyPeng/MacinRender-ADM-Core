//! The offline driver (PCM consistency 4.5): reads the input in blocks, renders them with a
//! prepared `PcmMixer` plan (loudspeakers) or HOA encoder plan and meters the written frames,
//! moved from the C++ `render_speaker_pcm` and HOA `render_window` with the same block size,
//! window and pre-roll rules, checkpoints and meter order. The caller steps it one block at a
//! time: between `render` and `commit` it writes the emitted frames, and it owns cancellation
//! and progress reporting. Triple Balance's size and D-mode processors (4.6) run inside the
//! driver after the gain mix, as the C++ block processor did.
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::ear_post::{FilterBank, Processor};
use mradm_dsp::hoa::{self, Encoder, MeterPreprocessor};
use mradm_dsp::meter::{Meter, MeterChannel, MeterMode};
use mradm_dsp::pcm_mix::{self, Mixer};
use mradm_dsp::triple_balance::{d_mode, session};
use mradm_wav::Reader;
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};
use std::thread::JoinHandle;

/// Where the caller attaches its locator to an error: none, `path=<input>` (WAVE reader errors,
/// as the C++ reader reported them), `input=<input>` (a failed render step), `meter` or
/// `HOA DSP` (the C++ HOA wrapper's context) or `Triple Balance DSP`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Location {
    None,
    Path,
    Input,
    Meter,
    HoaDsp,
    TripleBalanceDsp,
    /// The room runtime (`room-binaural`).
    Room,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub message: String,
    pub location: Location,
}

impl Error {
    pub(crate) fn wav(e: mradm_wav::Error) -> Self {
        Self {
            code: if (1..=6).contains(&e.code) { e.code } else { 6 },
            message: e.message,
            location: Location::Path,
        }
    }
    /// The C++ `catch (std::exception)`: `<prefix><what>` at `input=`.
    fn failed(prefix: &str, e: impl std::fmt::Display) -> Self {
        Self {
            code: 3,
            message: format!("{prefix}{e}"),
            location: Location::Input,
        }
    }
    pub(crate) fn dsp(e: &mradm_dsp::Error, location: Location) -> Self {
        Self {
            code: dsp_code(e),
            message: e.to_string(),
            location,
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// A frame range `[start, start + count)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Window {
    pub start: u64,
    pub count: u64,
}

/// A prepared plan: loudspeaker gains, the HOA3 encoder, EAR's direct and diffuse gains with
/// its decorrelation filters, or Triple Balance's gains with its size or D-mode processor.
#[derive(Clone)]
pub enum Kernel {
    Mix(Arc<pcm_mix::Plan>),
    Hoa(Arc<hoa::Plan>),
    Ear(Arc<EarPlan>),
    TripleBalance(Arc<TbPlan>),
}

/// Triple Balance: the point and bed gains, then the size mix of the `sized` tracks; or, in D
/// mode, a silent mix and the processor that owns every route.
pub enum TbPlan {
    Standard {
        mix: Arc<pcm_mix::Plan>,
        session: Arc<session::Plan>,
        sized: bool,
    },
    D {
        mix: Arc<pcm_mix::Plan>,
        plan: Arc<d_mode::Plan>,
    },
}

/// The stateful Triple Balance stage after the gain mix.
#[allow(clippy::large_enum_variant)] // one per driver or stream
pub(crate) enum TbStage {
    Size(session::Session),
    D(d_mode::Session, Arc<d_mode::Plan>),
}

impl TbStage {
    fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        start: u64,
        frames: usize,
        last: bool,
    ) -> mradm_dsp::Result<()> {
        match self {
            Self::Size(s) => s.process(input, output, &[], start, frames, last),
            Self::D(s, _) => s.process(input, output, start, frames),
        }
    }

    fn reset(&mut self) {
        match self {
            Self::Size(s) => s.reset(),
            Self::D(s, plan) => *s = d_mode::Session::new(Arc::clone(plan)),
        }
    }
}

pub struct EarPlan {
    pub mix: Arc<pcm_mix::Plan>,
    pub filters: Arc<FilterBank>,
    /// `wav71` renders in EAR's 0+7+0 order (L R C LFE Ls Rs Lrs Rrs) and is written in WAVE_7_1
    /// order (L R C LFE Lrs Rrs Ls Rs).
    pub wav71: bool,
}

/// The block renderer of a kernel.
#[allow(clippy::large_enum_variant)] // one per driver or stream
pub(crate) enum Engine {
    Mix(Mixer),
    Hoa(Encoder),
    Ear {
        mixer: Mixer,
        post: Processor,
        diffuse: Vec<f32>,
        wav71: bool,
    },
    TripleBalance {
        mixer: Mixer,
        stage: Option<TbStage>,
    },
}

impl Engine {
    pub(crate) fn new(
        kernel: &Kernel,
        block_size: u64,
        interpolation: u64,
        smoothing: bool,
    ) -> mradm_dsp::Result<Self> {
        Ok(match kernel {
            Kernel::Mix(plan) => Self::Mix(Mixer::new(
                Arc::clone(plan),
                block_size as usize,
                interpolation,
                smoothing,
            )?),
            Kernel::Hoa(plan) => Self::Hoa(Encoder::new(
                Arc::clone(plan),
                block_size as usize,
                interpolation,
                smoothing,
            )?),
            Kernel::Ear(plan) => {
                let post = Processor::new(Arc::clone(&plan.filters), block_size as usize)?;
                Self::Ear {
                    mixer: Mixer::new(
                        Arc::clone(&plan.mix),
                        block_size as usize,
                        interpolation,
                        smoothing,
                    )?,
                    post,
                    diffuse: vec![0.; plan.filters.channels() * block_size as usize],
                    wav71: plan.wav71,
                }
            }
            Kernel::TripleBalance(plan) => {
                let (mix, stage) = match plan.as_ref() {
                    TbPlan::Standard {
                        mix,
                        session,
                        sized,
                    } => (
                        mix,
                        sized
                            .then(|| {
                                session::Session::new(
                                    Arc::clone(session),
                                    block_size as usize,
                                    interpolation,
                                    false,
                                )
                            })
                            .transpose()?
                            .map(TbStage::Size),
                    ),
                    TbPlan::D { mix, plan } => (
                        mix,
                        Some(TbStage::D(
                            d_mode::Session::new(Arc::clone(plan)),
                            Arc::clone(plan),
                        )),
                    ),
                };
                Self::TripleBalance {
                    mixer: Mixer::new(
                        Arc::clone(mix),
                        block_size as usize,
                        interpolation,
                        smoothing,
                    )?,
                    stage,
                }
            }
        })
    }

    /// Adds the block to the zeroed `output` (EAR overwrites it).
    pub(crate) fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        start: u64,
        frames: usize,
    ) -> mradm_dsp::Result<()> {
        match self {
            Self::Mix(m) | Self::TripleBalance { mixer: m, .. } => {
                m.speaker(input, output, &[], start, frames, None, None)
            }
            Self::Hoa(e) => e.process(input, output, start, frames),
            Self::Ear {
                mixer,
                post,
                diffuse,
                wav71,
            } => {
                mixer.ear(input, output, diffuse, start, frames)?;
                post.process(output, diffuse, frames)?;
                let outputs = output.len() / frames.max(1);
                if *wav71 && outputs == 8 {
                    for frame in output.as_chunks_mut::<8>().0 {
                        frame.swap(4, 6);
                        frame.swap(5, 7);
                    }
                }
                Ok(())
            }
        }
    }

    /// The mixer forgets its cursors; the HOA encoder also seeds its diffuse delay lines at
    /// `start`; EAR also clears its decorrelation and delay state.
    pub(crate) fn reset(&mut self, start: u64) {
        match self {
            Self::Mix(m) => m.reset(),
            Self::Hoa(e) => e.reset(start),
            Self::Ear { mixer, post, .. } => {
                post.reset();
                mixer.reset();
            }
            Self::TripleBalance { mixer, stage } => {
                mixer.reset();
                if let Some(stage) = stage {
                    stage.reset();
                }
            }
        }
    }

    /// Triple Balance's size or D-mode stage, run on the mixed block after its checkpoints; its
    /// errors carry the C++ `Triple Balance DSP` context.
    pub(crate) fn finish_block(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        start: u64,
        frames: usize,
        last: bool,
    ) -> Result<()> {
        if let Self::TripleBalance {
            stage: Some(stage), ..
        } = self
        {
            stage
                .process(input, output, start, frames, last)
                .map_err(|e| Error::dsp(&e, Location::TripleBalanceDsp))?;
        }
        Ok(())
    }

    /// A stateful stage needs every block from frame 0, so a window still renders from there.
    pub(crate) fn is_stateful(&self) -> bool {
        matches!(self, Self::TripleBalance { stage: Some(_), .. })
    }

    pub(crate) fn is_hoa(&self) -> bool {
        matches!(self, Self::Hoa(_))
    }

    pub(crate) fn is_ear(&self) -> bool {
        matches!(self, Self::Ear { .. })
    }

    /// The C++ HOA errors carried the wrapper's context.
    pub(crate) fn location(&self) -> Location {
        if self.is_hoa() {
            Location::HoaDsp
        } else {
            Location::None
        }
    }
}

pub const HOA_SHORT_READ: &str = "short input read while encoding HOA";

pub struct Settings {
    pub kernel: Kernel,
    pub inputs: usize,
    pub outputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub default_interp_ms: u32,
    pub smoothing_frames: u64,
    /// Only frames inside it are written and metered.
    pub window: Option<Window>,
    /// Metered frames when there is no output window.
    pub meter_window: Option<Window>,
    /// Backend name for failure messages.
    pub backend: String,
}

/// One rendered block. `emit_offset` / `emit_frames` are the frames to write.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Block {
    pub start: u64,
    pub frames: usize,
    pub last: bool,
    pub emit_offset: usize,
    pub emit_frames: usize,
}

/// The rendering progress after a committed block, as the C++ loop reported it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Progress {
    pub fraction: f64,
    pub stage_fraction: f64,
    pub done: u64,
    pub total: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Metrics {
    pub lufs: Option<f64>,
    pub peak_dbtp: Option<f64>,
}

/// One block's input and output, interleaved.
type Buffers = (Vec<f32>, Vec<f32>);
/// The buffers, the metered frames `[offset, offset + frames)` and the block start.
type Job = (Buffers, usize, usize, u64);
type Done = (Buffers, std::result::Result<(), mradm_dsp::Error>);

/// The loudness and true peak of the written frames. The loudspeaker meter reads the output; the
/// HOA meter decodes it to the 7.1.4 AllRAD reference with the LFE channels taken out, which a
/// mono true-peak meter measures on their own.
#[allow(clippy::large_enum_variant)] // one per driver
enum Measure {
    Speaker(Meter),
    Hoa {
        pre: MeterPreprocessor,
        meter: Meter,
        lfe: Option<Meter>,
        decoded: Vec<f32>,
        lfe_mix: Vec<f32>,
    },
}

/// The 7.1.4 decode order; its LFE slot does not count towards loudness.
const DECODE_MAP: [MeterChannel; hoa::DECODE_CHANNELS] = [
    MeterChannel::Left,
    MeterChannel::Right,
    MeterChannel::Center,
    MeterChannel::Unused,
    MeterChannel::SideLeft,
    MeterChannel::SideRight,
    MeterChannel::RearLeft,
    MeterChannel::RearRight,
    MeterChannel::TopFrontLeft,
    MeterChannel::TopFrontRight,
    MeterChannel::TopRearLeft,
    MeterChannel::TopRearRight,
];

impl Measure {
    fn add(
        &mut self,
        input: &[f32],
        output: &[f32],
        start: u64,
        frames: usize,
    ) -> mradm_dsp::Result<()> {
        match self {
            Self::Speaker(meter) => meter.add_frames(output),
            Self::Hoa {
                pre,
                meter,
                lfe,
                decoded,
                lfe_mix,
            } => {
                pre.process(input, output, decoded, lfe_mix, start, frames)?;
                if let Some(lfe) = lfe {
                    lfe.add_frames(&lfe_mix[..frames])?;
                }
                meter.add_frames(&decoded[..frames * hoa::DECODE_CHANNELS])
            }
        }
    }

    fn metrics(&self) -> Metrics {
        let (meter, lfe) = match self {
            Self::Speaker(meter) => (meter, None),
            Self::Hoa { meter, lfe, .. } => (meter, lfe.as_ref()),
        };
        let lufs = meter.integrated().ok().filter(|v| v.is_finite());
        let mut peak = meter.max_true_peak().unwrap_or(0.);
        if let Some(p) = lfe.and_then(|m| m.max_true_peak().ok())
            && peak < p
        {
            peak = p;
        }
        let peak_dbtp = (peak > 0.).then(|| mradm_dsp::post_gain::amplitude_to_db(peak));
        Metrics { lufs, peak_dbtp }
    }
}

/// The meter on one background thread. Blocks are measured strictly in submission order, so the
/// result equals inline metering; buffers come back for reuse.
struct MeterWorker {
    jobs: Option<SyncSender<Job>>,
    done: Receiver<Done>,
    thread: Option<JoinHandle<Measure>>,
    outstanding: usize,
}

impl MeterWorker {
    fn new(mut measure: Measure, inputs: usize, outputs: usize) -> Self {
        let (jobs, inbox) = sync_channel::<Job>(2);
        let (outbox, done) = sync_channel::<Done>(2);
        let thread = std::thread::spawn(move || {
            for ((input, output), offset, frames, start) in inbox {
                let result = measure.add(
                    &input[offset * inputs..(offset + frames) * inputs],
                    &output[offset * outputs..(offset + frames) * outputs],
                    start + offset as u64,
                    frames,
                );
                if outbox.send(((input, output), result)).is_err() {
                    break;
                }
            }
            measure
        });
        Self {
            jobs: Some(jobs),
            done,
            thread: Some(thread),
            outstanding: 0,
        }
    }

    fn submit(&mut self, job: Job) {
        self.outstanding += 1;
        // The worker only stops when this side drops the sender.
        let _ = self.jobs.as_ref().map(|j| j.send(job));
    }

    /// The oldest measured buffers.
    fn reclaim(&mut self) -> Option<Done> {
        if self.outstanding == 0 {
            return None;
        }
        self.outstanding -= 1;
        self.done.recv().ok()
    }

    fn finish(mut self) -> Option<Measure> {
        self.jobs = None;
        self.thread.take()?.join().ok()
    }
}

/// The first rendered frame of a window starting at `target`, on the absolute block grid that
/// object smoothing interpolates across. Loudspeaker mixing is stateless at block edges, so a
/// window starts at its own block. HOA's 1024-frame diffuse delay line and EAR's decorrelation
/// overlap and compensation delay are refilled by one aligned pre-roll block. A stateful Triple
/// Balance stage needs every block from frame 0. File-stream seeks start here too (4.9).
pub(crate) fn window_start(engine: &Engine, target: u64, block_size: u64) -> u64 {
    if engine.is_hoa() || engine.is_ear() {
        if target >= block_size {
            (target / block_size - 1) * block_size
        } else {
            0
        }
    } else if engine.is_stateful() {
        0
    } else {
        target / block_size * block_size
    }
}

pub struct Driver {
    reader: Reader<BufReader<File>>,
    engine: Engine,
    inputs: usize,
    outputs: usize,
    frames: u64,
    block_size: u64,
    /// Double-buffered blocks; a slot is empty while the meter still reads it.
    buffers: [Option<Buffers>; 2],
    slot: usize,
    meter: MeterWorker,
    /// The C++ failure prefix: `<backend> render failed: ` or `HOA encode failed: `.
    failure: String,
    window: Option<Window>,
    meter_window: Option<Window>,
    win_start: u64,
    win_end: u64,
    start_pos: u64,
    done: u64,
    current: Option<Block>,
}

impl Driver {
    /// Opens `path` as the C++ reader did; reader errors carry `Location::Path`.
    pub fn open_reader(path: &str) -> Result<Reader<BufReader<File>>> {
        let file = File::open(path).map_err(|e| Error::wav(e.into()))?;
        Reader::new(BufReader::new(file)).map_err(Error::wav)
    }

    pub fn new(reader: Reader<BufReader<File>>, settings: Settings) -> Result<Self> {
        let (inputs, outputs) = (settings.inputs, settings.outputs);
        let default_interp =
            u64::from(settings.sample_rate) * u64::from(settings.default_interp_ms) / 1000;
        let block_size = settings.smoothing_frames.max(1024);
        let meter_error = |e: mradm_dsp::Error| Error::dsp(&e, Location::Meter);
        let (engine, measure, failure) = match &settings.kernel {
            Kernel::Mix(_) | Kernel::Ear(_) | Kernel::TripleBalance(_) => {
                let meter = Meter::new(
                    outputs as u32,
                    settings.sample_rate,
                    MeterMode::IntegratedTruePeak,
                    &[],
                )
                .map_err(meter_error)?;
                let failure = if matches!(settings.kernel, Kernel::Ear(_)) {
                    "render failed: ".to_owned()
                } else {
                    format!("{} render failed: ", settings.backend)
                };
                let engine = Engine::new(
                    &settings.kernel,
                    block_size,
                    default_interp,
                    settings.smoothing_frames > 0,
                )
                .map_err(|e| Error::failed(&failure, e))?;
                (engine, Measure::Speaker(meter), failure)
            }
            Kernel::Hoa(plan) => {
                let hoa_error = |e: mradm_dsp::Error| Error::dsp(&e, Location::HoaDsp);
                let engine = Engine::new(
                    &settings.kernel,
                    block_size,
                    default_interp,
                    settings.smoothing_frames > 0,
                )
                .map_err(hoa_error)?;
                let pre =
                    MeterPreprocessor::new(Arc::clone(plan), block_size as usize, default_interp)
                        .map_err(hoa_error)?;
                let meter = Meter::new(
                    hoa::DECODE_CHANNELS as u32,
                    settings.sample_rate,
                    MeterMode::IntegratedTruePeak,
                    &DECODE_MAP,
                )
                .map_err(meter_error)?;
                let lfe = if plan.has_lfe() {
                    Some(
                        Meter::new(1, settings.sample_rate, MeterMode::TruePeak, &[])
                            .map_err(meter_error)?,
                    )
                } else {
                    None
                };
                let measure = Measure::Hoa {
                    pre,
                    meter,
                    lfe,
                    decoded: vec![0.; hoa::DECODE_CHANNELS * block_size as usize],
                    lfe_mix: vec![0.; block_size as usize],
                };
                (engine, measure, "HOA encode failed: ".to_owned())
            }
        };
        let frames = settings.frames;
        let (win_start, win_end) = match settings.window {
            Some(w) => {
                let start = w.start.min(frames);
                (start, start.saturating_add(w.count).min(frames))
            }
            None => (0, frames),
        };
        let start_pos = match settings.window {
            None => 0,
            Some(_) => window_start(&engine, win_start, block_size),
        };
        let buffers = || {
            Some((
                vec![0.; inputs * block_size as usize],
                vec![0.; outputs * block_size as usize],
            ))
        };
        let mut driver = Self {
            reader,
            engine,
            inputs,
            outputs,
            frames,
            block_size,
            buffers: [buffers(), buffers()],
            slot: 0,
            meter: MeterWorker::new(measure, inputs, outputs),
            failure,
            window: settings.window,
            meter_window: settings.meter_window,
            win_start,
            win_end,
            start_pos,
            done: start_pos,
            current: None,
        };
        if start_pos > 0 {
            driver.reader.seek_frame(start_pos).map_err(Error::wav)?;
            if driver.engine.is_hoa() {
                driver.engine.reset(start_pos);
            }
        }
        Ok(driver)
    }

    /// Reads and renders the next block, or `None` after the last one.
    pub fn render(&mut self) -> Result<Option<Block>> {
        if self.done >= self.win_end {
            return Ok(None);
        }
        let frames = self.block_size.min(self.frames - self.done) as usize;
        let out_samples = self.outputs * frames;
        // Reclaim this buffer once the meter has finished its previous use of it.
        while self.buffers[self.slot].is_none() {
            let (buffers, result) = self
                .meter
                .reclaim()
                .ok_or_else(|| Error::failed(&self.failure, "meter worker stopped"))?;
            result.map_err(|e| Error::failed(&self.failure, e))?;
            let free = self
                .buffers
                .iter()
                .position(Option::is_none)
                .unwrap_or(self.slot);
            self.buffers[free] = Some(buffers);
        }
        let (input, output) = self.buffers[self.slot].as_mut().expect("reclaimed above");
        let input = &mut input[..self.inputs * frames];
        let read = self.reader.read_frames(input).map_err(Error::wav)?;
        if read != frames as u64 {
            return Err(if self.engine.is_hoa() {
                Error {
                    code: 3,
                    message: HOA_SHORT_READ.to_owned(),
                    location: Location::Input,
                }
            } else {
                Error {
                    code: 3,
                    message: if self.engine.is_ear() {
                        "short render input read"
                    } else {
                        "short input read while rendering speaker PCM"
                    }
                    .to_owned(),
                    location: Location::None,
                }
            });
        }
        output[..out_samples].fill(0.);
        let output = &mut output[..out_samples];
        let location = self.engine.location();
        self.engine
            .process(input, output, self.done, frames)
            .map_err(|e| match location {
                Location::HoaDsp => Error::dsp(&e, location),
                _ => Error::failed(&self.failure, e),
            })?;
        if ENABLED && matches!(self.engine, Engine::Mix(_) | Engine::TripleBalance { .. }) {
            let key = format!("speaker/s{}", self.done);
            let _ = process(&format!("{key}.10-input.f32"), Words::F32(input));
            let _ = process(&format!("{key}.40-mix.f32"), Words::F32(output));
        }
        let end = self.done + frames as u64;
        self.engine
            .finish_block(input, output, self.done, frames, end == self.frames)?;
        let (lo, hi) = (self.done.max(self.win_start), end.min(self.win_end));
        let block = Block {
            start: self.done,
            frames,
            last: end == self.frames,
            emit_offset: if hi > lo {
                (lo - self.done) as usize
            } else {
                0
            },
            emit_frames: if hi > lo { (hi - lo) as usize } else { 0 },
        };
        self.current = Some(block);
        Ok(Some(block))
    }

    /// The current block's input, interleaved.
    pub fn input(&self) -> &[f32] {
        let frames = self.current.map_or(0, |b| b.frames);
        match self.buffers[self.slot].as_ref() {
            Some((input, _)) => &input[..self.inputs * frames],
            None => &[],
        }
    }

    /// The current block's output, interleaved.
    pub fn output(&mut self) -> &mut [f32] {
        let samples = self.current.map_or(0, |b| b.frames) * self.outputs;
        match self.buffers[self.slot].as_mut() {
            Some((_, output)) => &mut output[..samples],
            None => &mut [],
        }
    }

    /// Meters the current block's written frames and moves on.
    pub fn commit(&mut self) -> Result<Progress> {
        let block = self
            .current
            .take()
            .ok_or_else(|| Error::failed(&self.failure, "no block to commit"))?;
        let (offset, count) = if self.window.is_some() {
            (block.emit_offset, block.emit_frames)
        } else {
            chunk(self.meter_window, block.start, block.frames as u64)
        };
        if count > 0 {
            let buffers = self.buffers[self.slot]
                .take()
                .expect("current block buffer");
            self.meter.submit((buffers, offset, count, block.start));
        }
        self.done += block.frames as u64;
        self.slot = (self.slot + 1) % 2;
        let total = (self.win_end - self.start_pos).max(1);
        let done = self.done.min(self.win_end) - self.start_pos;
        let stage_fraction = done as f64 / total as f64;
        Ok(Progress {
            fraction: 0.3 + 0.6 * stage_fraction,
            stage_fraction,
            done,
            total,
        })
    }

    /// Waits for the outstanding measurements and reads the global loudness and true peak.
    pub fn finish(mut self) -> Result<Metrics> {
        while let Some((_, result)) = self.meter.reclaim() {
            result.map_err(|e| Error::failed(&self.failure, e))?;
        }
        let measure = self
            .meter
            .finish()
            .ok_or_else(|| Error::failed(&self.failure, "meter worker stopped"))?;
        Ok(measure.metrics())
    }
}
pub(crate) fn dsp_code(e: &mradm_dsp::Error) -> i32 {
    match e {
        mradm_dsp::Error::InvalidArgument(_) => 1,
        mradm_dsp::Error::Unsupported(_) => 2,
        mradm_dsp::Error::Io(_) => 3,
        mradm_dsp::Error::RenderFailed(_) => 4,
    }
}

/// The part of a block `[start, start + len)` inside the meter window (all of it without one).
fn chunk(window: Option<Window>, start: u64, len: u64) -> (usize, usize) {
    let Some(w) = window else {
        return (0, len as usize);
    };
    let (lo, hi) = (start.max(w.start), (start + len).min(w.start + w.count));
    if hi <= lo {
        (0, 0)
    } else {
        ((lo - start) as usize, (hi - lo) as usize)
    }
}
