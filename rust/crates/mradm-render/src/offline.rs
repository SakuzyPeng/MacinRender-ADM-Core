//! The offline loudspeaker driver (PCM consistency 4.5): reads the input in blocks, mixes them
//! with a prepared `PcmMixer` plan and meters the written frames, moved from the C++
//! `render_speaker_pcm` with the same block size, window rules, checkpoints and meter order.
//! The caller steps it one block at a time: between `render` and `commit` it may run stateful DSP
//! on the block and writes the emitted frames, and it owns cancellation and progress reporting.
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::meter::{Meter, MeterMode};
use mradm_dsp::pcm_mix::{Mixer, Plan};
use mradm_wav::Reader;
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};
use std::thread::JoinHandle;

/// Where the caller attaches its locator to an error: none, `path=<input>` (WAVE reader errors,
/// as the C++ reader reported them), `input=<input>` (a failed render step) or `meter`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Location {
    None,
    Path,
    Input,
    Meter,
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
    /// The C++ `catch (std::exception)`: `<backend> render failed: <what>` at `input=`.
    fn failed(backend: &str, e: impl std::fmt::Display) -> Self {
        Self {
            code: 3,
            message: format!("{backend} render failed: {e}"),
            location: Location::Input,
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

pub struct Settings {
    pub plan: Arc<Plan>,
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
    /// Stateful DSP runs between `render` and `commit`, so a window still renders from frame 0.
    pub stateful: bool,
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

type Job = (Vec<f32>, usize, usize);
type Done = (Vec<f32>, std::result::Result<(), mradm_dsp::Error>);

/// The loudness and true-peak meter on one background thread. Blocks are measured strictly in
/// submission order, so the result equals inline metering; buffers come back for reuse.
struct MeterWorker {
    jobs: Option<SyncSender<Job>>,
    done: Receiver<Done>,
    thread: Option<JoinHandle<Meter>>,
    outstanding: usize,
}

impl MeterWorker {
    fn new(mut meter: Meter, channels: usize) -> Self {
        let (jobs, inbox) = sync_channel::<Job>(2);
        let (outbox, done) = sync_channel::<Done>(2);
        let thread = std::thread::spawn(move || {
            for (buffer, offset, frames) in inbox {
                let span = &buffer[offset * channels..(offset + frames) * channels];
                let result = meter.add_frames(span);
                if outbox.send((buffer, result)).is_err() {
                    break;
                }
            }
            meter
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

    /// The oldest measured buffer.
    fn reclaim(&mut self) -> Option<Done> {
        if self.outstanding == 0 {
            return None;
        }
        self.outstanding -= 1;
        self.done.recv().ok()
    }

    fn finish(mut self) -> Option<Meter> {
        self.jobs = None;
        self.thread.take()?.join().ok()
    }
}

pub struct Driver {
    reader: Reader<BufReader<File>>,
    mixer: Mixer,
    inputs: usize,
    outputs: usize,
    frames: u64,
    block_size: u64,
    input: Vec<f32>,
    /// Double-buffered output; a slot is empty while the meter still reads it.
    buffers: [Option<Vec<f32>>; 2],
    slot: usize,
    meter: MeterWorker,
    backend: String,
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
        let outputs = settings.outputs;
        let meter = Meter::new(
            outputs as u32,
            settings.sample_rate,
            MeterMode::IntegratedTruePeak,
            &[],
        )
        .map_err(|e| Error {
            code: dsp_code(&e),
            message: e.to_string(),
            location: Location::Meter,
        })?;
        let default_interp =
            u64::from(settings.sample_rate) * u64::from(settings.default_interp_ms) / 1000;
        let block_size = settings.smoothing_frames.max(1024);
        let mixer = Mixer::new(
            settings.plan,
            block_size as usize,
            default_interp,
            settings.smoothing_frames > 0,
        )
        .map_err(|e| Error::failed(&settings.backend, e))?;
        let frames = settings.frames;
        let (win_start, win_end) = match settings.window {
            Some(w) => {
                let start = w.start.min(frames);
                (start, start.saturating_add(w.count).min(frames))
            }
            None => (0, frames),
        };
        let start_pos = if settings.window.is_some() && !settings.stateful {
            win_start / block_size * block_size
        } else {
            0
        };
        let mut driver = Self {
            reader,
            mixer,
            inputs: settings.inputs,
            outputs,
            frames,
            block_size,
            input: vec![0.; settings.inputs * block_size as usize],
            buffers: [
                Some(vec![0.; outputs * block_size as usize]),
                Some(vec![0.; outputs * block_size as usize]),
            ],
            slot: 0,
            meter: MeterWorker::new(meter, outputs),
            backend: settings.backend,
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
        }
        Ok(driver)
    }

    /// Reads and mixes the next block, or `None` after the last one.
    pub fn render(&mut self) -> Result<Option<Block>> {
        if self.done >= self.win_end {
            return Ok(None);
        }
        let frames = self.block_size.min(self.frames - self.done) as usize;
        let out_samples = self.outputs * frames;
        // Reclaim this buffer once the meter has finished its previous use of it.
        while self.buffers[self.slot].is_none() {
            let (buffer, result) = self
                .meter
                .reclaim()
                .ok_or_else(|| Error::failed(&self.backend, "meter worker stopped"))?;
            result.map_err(|e| Error::failed(&self.backend, e))?;
            let free = self
                .buffers
                .iter()
                .position(Option::is_none)
                .unwrap_or(self.slot);
            self.buffers[free] = Some(buffer);
        }
        let input = &mut self.input[..self.inputs * frames];
        let read = self.reader.read_frames(input).map_err(Error::wav)?;
        if read != frames as u64 {
            return Err(Error {
                code: 3,
                message: "short input read while rendering speaker PCM".to_owned(),
                location: Location::None,
            });
        }
        let output = self.buffers[self.slot].as_mut().expect("reclaimed above");
        output[..out_samples].fill(0.);
        self.mixer
            .speaker(input, output, &[], self.done, frames, None, None)
            .map_err(|e| Error::failed(&self.backend, e))?;
        if ENABLED {
            let key = format!("speaker/s{}", self.done);
            let _ = process(&format!("{key}.10-input.f32"), Words::F32(input));
            let _ = process(
                &format!("{key}.40-mix.f32"),
                Words::F32(&output[..out_samples]),
            );
        }
        let end = self.done + frames as u64;
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
        &self.input[..self.inputs * frames]
    }

    /// The current block's output, interleaved.
    pub fn output(&mut self) -> &mut [f32] {
        let samples = self.current.map_or(0, |b| b.frames) * self.outputs;
        match self.buffers[self.slot].as_mut() {
            Some(buffer) => &mut buffer[..samples],
            None => &mut [],
        }
    }

    /// Meters the current block's written frames and moves on.
    pub fn commit(&mut self) -> Result<Progress> {
        let block = self
            .current
            .take()
            .ok_or_else(|| Error::failed(&self.backend, "no block to commit"))?;
        let (offset, count) = if self.window.is_some() {
            (block.emit_offset, block.emit_frames)
        } else {
            chunk(self.meter_window, block.start, block.frames as u64)
        };
        if count > 0 {
            let buffer = self.buffers[self.slot]
                .take()
                .expect("current block buffer");
            self.meter.submit((buffer, offset, count));
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
            result.map_err(|e| Error::failed(&self.backend, e))?;
        }
        let meter = self
            .meter
            .finish()
            .ok_or_else(|| Error::failed(&self.backend, "meter worker stopped"))?;
        let lufs = meter.integrated().ok().filter(|v| v.is_finite());
        let peak_dbtp = meter
            .max_true_peak()
            .ok()
            .filter(|&p| p > 0.)
            .map(mradm_dsp::post_gain::amplitude_to_db);
        Ok(Metrics { lufs, peak_dbtp })
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
