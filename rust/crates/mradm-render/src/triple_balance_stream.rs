//! Triple Balance file streaming (PCM consistency 4.6), moved from the C++ `TripleBalanceStream`
//! with the same block order and state rules: 1024-frame blocks into a FIFO; point and bed
//! channels through the gain mixer (a canonical track at size scale 0 through its point curve),
//! then the size mix; live gain through a 20 ms de-zipper; size scales, which reach the next
//! render block; and an LRU of size-state checkpoints that seeks restore before rendering forward
//! to the target. The caller steps a seek block by block and owns its cancellation.
use crate::offline::{Driver, Error, Location, Result, TbPlan};
use crate::routing::{self, Override};
use crate::triple_balance::SizeTrack;
use crate::vbap::Binding;
use mradm_dsp::gain::GainBank;
use mradm_dsp::pcm_mix::{self, Mixer};
use mradm_dsp::stream_dsp::live_gain;
use mradm_dsp::triple_balance::session::{self, Session, Snapshot};
use mradm_wav::Reader;
use std::collections::VecDeque;
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;

const BLOCK_FRAMES: u64 = 1024;
/// The live gain de-zipper (`LiveGainRamp::k_default_ramp_ms`).
const RAMP_MS: u32 = 20;
pub const DEFAULT_CHECKPOINT_BUDGET: usize = 32 * 1024 * 1024;
/// The bytes the C++ stream counted per checkpoint besides the size state: its `Checkpoint`
/// (frame and handle with deleter) and the `std::list` node's two links.
const CHECKPOINT_OVERHEAD: usize = size_of::<u64>() + 4 * size_of::<usize>();

/// One live edit (`LiveObjectOverride`).
#[derive(Clone, Copy, Debug)]
pub struct LiveOverride<'a> {
    pub base: Override<'a>,
    pub extent_scale: f32,
    pub extent_width_scale: f32,
    pub extent_height_scale: f32,
    pub extent_depth_scale: f32,
    pub diffuse_scale: f32,
    pub divergence_scale: f32,
}

/// The immutable standard-mode preparation the stream renders and validates.
#[derive(Clone)]
pub struct Source {
    pub mix: Arc<pcm_mix::Plan>,
    pub session: Arc<session::Plan>,
    pub bindings: Arc<[Binding]>,
    pub size_tracks: Arc<[SizeTrack]>,
    pub spread_none: bool,
    pub inputs: usize,
    pub outputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
}

impl Source {
    pub fn of(plan: &TbPlan) -> Option<(Arc<pcm_mix::Plan>, Arc<session::Plan>)> {
        match plan {
            TbPlan::Standard { mix, session, .. } => Some((Arc::clone(mix), Arc::clone(session))),
            TbPlan::D { .. } => None,
        }
    }

    /// A size track needs state rendered from the file start.
    pub fn requires_preroll(&self) -> bool {
        !self.size_tracks.is_empty()
    }

    /// Rejects edits beyond gain, mute and linked isotropic size, as the C++ stream did.
    pub fn validate_overrides(
        &self,
        overrides: &[LiveOverride],
    ) -> std::result::Result<(), (i32, String, Vec<u8>)> {
        for item in overrides {
            let o = &item.base;
            let width = item.extent_scale * item.extent_width_scale;
            let height = item.extent_scale * item.extent_height_scale;
            let depth = item.extent_scale * item.extent_depth_scale;
            let reject =
                |code: i32, message: &str| Err((code, message.to_owned(), o.object_id.to_vec()));
            if !o.gain_db.is_finite()
                || !live_gain(o.gain_db).is_finite()
                || !width.is_finite()
                || !height.is_finite()
                || !depth.is_finite()
                || item.extent_scale < 0.
                || item.extent_width_scale < 0.
                || item.extent_height_scale < 0.
                || item.extent_depth_scale < 0.
            {
                return reject(1, "Triple Balance requires finite nonnegative size scales");
            }
            if width != height
                || width != depth
                || item.diffuse_scale != 1.
                || item.divergence_scale != 1.
                || o.head_locked.unwrap_or(false)
            {
                return reject(
                    2,
                    "Triple Balance live editing supports gain, mute and linked isotropic size only",
                );
            }
            let bed = self
                .bindings
                .iter()
                .any(|b| b.object_id == o.object_id && !b.label_key.is_empty());
            if width != 1. && (bed || !o.speaker_label.is_empty()) {
                return reject(2, "Triple Balance size editing applies to Objects only");
            }
            if self.spread_none && width != 1. {
                return reject(2, "Triple Balance size editing requires spread=auto");
            }
            let pure_diffuse_point = self.size_tracks.iter().any(|t| {
                t.object_id == o.object_id && t.has_diffuse && t.minimum_diffuse_size * width == 0.
            });
            if pure_diffuse_point {
                return reject(
                    2,
                    "Triple Balance does not support a pure diffuse point source",
                );
            }
        }
        Ok(())
    }
}

struct Checkpoint {
    frame: u64,
    states: Snapshot,
}

pub struct Stream {
    source: Source,
    reader: Reader<BufReader<File>>,
    session: Session,
    gain_mix: Mixer,
    checkpoint_interval: u64,
    input: Vec<f32>,
    envelopes: Vec<f32>,
    gain_targets: Vec<f32>,
    ramps: GainBank,
    scales: Vec<f32>,
    fifo: Vec<f32>,
    /// Zero when cleared (before the first block and after a seek), as the C++ `fifo_.empty()`.
    fifo_len: usize,
    fifo_read: usize,
    position: u64,
    anchor: u64,
    canonical: bool,
    budget: usize,
    size_by_channel: Vec<usize>,
    /// Most recently used first.
    checkpoints: VecDeque<Checkpoint>,
    /// The boundary and target of the seek in progress.
    seeking: Option<(u64, u64)>,
}

fn failed(e: mradm_dsp::Error) -> Error {
    Error::dsp(&e, Location::TripleBalanceDsp)
}

fn invalid(message: &str) -> Error {
    Error {
        code: 1,
        message: message.to_owned(),
        location: Location::None,
    }
}

impl Stream {
    /// Opens `path` as the offline driver does; reader errors carry `Location::Path`.
    pub fn open(
        path: &str,
        source: Source,
        default_interp_ms: u32,
        checkpoint_budget: usize,
    ) -> Result<Self> {
        let reader = Driver::open_reader(path)?;
        let default_interp = u64::from(source.sample_rate) * u64::from(default_interp_ms) / 1000;
        let session = Session::new(
            Arc::clone(&source.session),
            BLOCK_FRAMES as usize,
            default_interp,
            true,
        )
        .map_err(failed)?;
        let gain_mix = Mixer::new(
            Arc::clone(&source.mix),
            BLOCK_FRAMES as usize,
            default_interp,
            false,
        )
        .map_err(failed)?;
        let rate = u64::from(source.sample_rate);
        let inputs = source.inputs;
        let mut size_by_channel = vec![source.size_tracks.len(); inputs];
        for (i, t) in source.size_tracks.iter().enumerate() {
            if let Some(slot) = size_by_channel.get_mut(usize::from(t.input_channel)) {
                *slot = i;
            }
        }
        let gain_targets = vec![1.; inputs];
        let mut ramps = GainBank::new(inputs, source.sample_rate, RAMP_MS).map_err(failed)?;
        ramps.set_targets(&gain_targets).map_err(failed)?;
        Ok(Self {
            reader,
            session,
            gain_mix,
            checkpoint_interval: rate.div_ceil(BLOCK_FRAMES) * BLOCK_FRAMES,
            input: vec![0.; BLOCK_FRAMES as usize * inputs],
            envelopes: vec![0.; BLOCK_FRAMES as usize * inputs],
            gain_targets,
            ramps,
            scales: vec![1.; source.size_tracks.len()],
            fifo: Vec::with_capacity(BLOCK_FRAMES as usize * source.outputs),
            fifo_len: 0,
            fifo_read: 0,
            position: 0,
            anchor: 0,
            canonical: true,
            budget: checkpoint_budget.min(DEFAULT_CHECKPOINT_BUDGET),
            size_by_channel,
            checkpoints: VecDeque::new(),
            seeking: None,
            source,
        })
    }

    pub fn outputs(&self) -> usize {
        self.source.outputs
    }

    /// A size track's state reaches the target only by rendering from the file start.
    pub fn requires_preroll(&self) -> bool {
        self.source.requires_preroll()
    }

    /// The bytes one checkpoint counts against the budget.
    pub fn checkpoint_bytes(&self) -> Result<usize> {
        Ok(CHECKPOINT_OVERHEAD + self.session.snapshot_bytes().map_err(failed)?)
    }

    /// Fills `output` (interleaved) with up to `frames` frames; fewer only at the end.
    pub fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize> {
        let outputs = self.source.outputs;
        if frames > output.len() / outputs {
            return Err(invalid("triple-balance: output buffer is too small"));
        }
        let mut produced = 0;
        while produced < frames {
            if self.fifo_read == self.fifo_len {
                if self.position == self.source.frames {
                    break;
                }
                self.render_block()?;
            }
            let take = (frames - produced).min((self.fifo_len - self.fifo_read) / outputs);
            output[produced * outputs..(produced + take) * outputs]
                .copy_from_slice(&self.fifo[self.fifo_read..self.fifo_read + take * outputs]);
            self.fifo_read += take * outputs;
            produced += take;
        }
        Ok(produced)
    }

    /// Validates against immutable preparation metadata.
    pub fn validate_overrides(
        &self,
        overrides: &[LiveOverride],
    ) -> std::result::Result<(), (i32, String, Vec<u8>)> {
        self.source.validate_overrides(overrides)
    }

    /// Ignores an update that `validate_overrides` rejects.
    pub fn set_overrides(&mut self, overrides: &[LiveOverride]) -> Result<()> {
        if self.validate_overrides(overrides).is_err() {
            return Ok(());
        }
        let base: Vec<Override> = overrides.iter().map(|o| o.base).collect();
        self.gain_targets.fill(1.);
        for b in self.source.bindings.iter() {
            if let Some(gain) = routing::live_channel_gain(&base, &b.object_id, &b.label_key) {
                self.gain_targets[usize::from(b.input_channel)] = gain;
            }
        }
        self.ramps.set_targets(&self.gain_targets).map_err(failed)?;
        let mut scales = vec![1.; self.scales.len()];
        for (i, track) in self.source.size_tracks.iter().enumerate() {
            for item in overrides {
                if item.base.object_id == track.object_id && item.base.speaker_label.is_empty() {
                    scales[i] = item.extent_scale * item.extent_width_scale;
                    // Nonnegative multiplication is monotonic, including underflow: this summary
                    // preserves the all-events-zero test without retaining the event table.
                    if track.maximum_source_size * scales[i] == 0. {
                        scales[i] = 0.;
                    }
                }
            }
        }
        if scales != self.scales {
            let initial = self.position == 0 && self.fifo_len == 0;
            self.scales = scales;
            self.checkpoints.clear();
            self.canonical = initial;
            if initial {
                self.session.set_matrix(&self.scales).map_err(failed)?;
            } else {
                // A formerly all-point track joins the size mix at the next render block. Keep
                // the verified point timeline for the one-control-block transition.
                let active = vec![1.; self.scales.len()];
                self.session.set_matrix(&active).map_err(failed)?;
            }
            self.session
                .set_scales(&self.scales, initial)
                .map_err(failed)?;
        }
        Ok(())
    }

    /// Starts a seek: back to the latest checkpoint at or before the target's block (else the
    /// file start, with size tracks). While it reports a pending block the caller may cancel,
    /// then calls `seek_step`; `seek_finish` ends the seek.
    pub fn seek_begin(&mut self, frame: u64) -> Result<bool> {
        let target = frame.min(self.source.frames);
        let boundary = target / BLOCK_FRAMES * BLOCK_FRAMES;
        self.session.set_matrix(&self.scales).map_err(failed)?;
        self.session.reset();
        self.session
            .set_scales(&self.scales, true)
            .map_err(failed)?;
        self.gain_mix.reset();
        self.position = if self.source.size_tracks.is_empty() {
            boundary
        } else {
            0
        };
        let best = self
            .checkpoints
            .iter()
            .enumerate()
            .filter(|(_, c)| c.frame <= boundary)
            .max_by_key(|(_, c)| c.frame)
            .map(|(i, _)| i);
        if let Some(i) = best {
            let checkpoint = self.checkpoints.remove(i).expect("found above");
            self.position = checkpoint.frame;
            self.session.restore(&checkpoint.states).map_err(failed)?;
            self.checkpoints.push_front(checkpoint);
        }
        self.fifo_len = 0;
        self.fifo_read = 0;
        self.canonical = true;
        self.ramps.reset();
        self.ramps.set_targets(&self.gain_targets).map_err(failed)?;
        self.reader.seek_frame(self.position).map_err(Error::wav)?;
        self.seeking = Some((boundary, target));
        Ok(self.position < boundary)
    }

    /// Renders one pending block towards the target's block; whether another is pending.
    pub fn seek_step(&mut self) -> Result<bool> {
        let (boundary, _) = self.seeking.ok_or_else(|| invalid("no seek in progress"))?;
        if self.position < boundary {
            self.render_block()?;
        }
        Ok(self.position < boundary)
    }

    /// Pins the seek anchor, keeps its checkpoint and renders the block holding the target.
    pub fn seek_finish(&mut self) -> Result<()> {
        let (boundary, target) = self
            .seeking
            .take()
            .ok_or_else(|| invalid("no seek in progress"))?;
        self.anchor = boundary;
        self.save_checkpoint(true)?;
        self.fifo_len = 0;
        self.fifo_read = 0;
        if target > self.position {
            self.render_block()?;
            self.fifo_read = (target - boundary) as usize * self.source.outputs;
        }
        Ok(())
    }

    /// The C++ LRU: one checkpoint per second-aligned block of a canonical run (and the forced
    /// seek anchor), the least recently used evicted first but never the anchor.
    fn save_checkpoint(&mut self, force: bool) -> Result<()> {
        if !self.canonical
            || self.source.size_tracks.is_empty()
            || self.position == 0
            || !self.position.is_multiple_of(BLOCK_FRAMES)
            || (!force && !self.position.is_multiple_of(self.checkpoint_interval))
        {
            return Ok(());
        }
        let capacity = self.budget / self.checkpoint_bytes()?;
        if capacity == 0 {
            return Ok(());
        }
        if let Some(i) = self
            .checkpoints
            .iter()
            .position(|c| c.frame == self.position)
        {
            let existing = self.checkpoints.remove(i).expect("found above");
            self.checkpoints.push_front(existing);
            return Ok(());
        }
        while self.checkpoints.len() >= capacity {
            let mut victim = self.checkpoints.len() - 1;
            if self.checkpoints[victim].frame == self.anchor {
                if self.checkpoints.len() == 1 && !force {
                    return Ok(());
                }
                if self.checkpoints.len() > 1 {
                    victim -= 1;
                }
            }
            self.checkpoints.remove(victim);
        }
        self.checkpoints.push_front(Checkpoint {
            frame: self.position,
            states: self.session.snapshot(),
        });
        Ok(())
    }

    fn render_block(&mut self) -> Result<()> {
        let inputs = self.source.inputs;
        let outputs = self.source.outputs;
        let frames = BLOCK_FRAMES.min(self.source.frames - self.position) as usize;
        let input = &mut self.input[..frames * inputs];
        let read = self.reader.read_frames(input).map_err(Error::wav)?;
        if read != frames as u64 {
            return Err(Error {
                code: 3,
                message: "short input read in Triple Balance stream".to_owned(),
                location: Location::None,
            });
        }
        self.ramps
            .fill(&mut self.envelopes[..frames * inputs])
            .map_err(failed)?;
        self.fifo.clear();
        self.fifo.resize(frames * outputs, 0.);
        self.fifo_len = self.fifo.len();
        self.fifo_read = 0;
        self.session
            .prepare_points(self.position, frames)
            .map_err(failed)?;
        for (i, b) in self.source.bindings.iter().enumerate() {
            let track = self.size_by_channel[usize::from(b.input_channel)];
            if self.canonical && track < self.scales.len() && self.scales[track] == 0. {
                self.session
                    .point(
                        track,
                        &self.input[..frames * inputs],
                        &mut self.fifo,
                        &self.envelopes,
                        self.position,
                        frames,
                        true,
                    )
                    .map_err(failed)?;
            } else {
                self.gain_mix
                    .speaker(
                        &self.input[..frames * inputs],
                        &mut self.fifo,
                        &self.envelopes,
                        self.position,
                        frames,
                        Some(i),
                        None,
                    )
                    .map_err(failed)?;
            }
        }
        self.session
            .process(
                &self.input[..frames * inputs],
                &mut self.fifo,
                &self.envelopes,
                self.position,
                frames,
                self.position + frames as u64 == self.source.frames,
            )
            .map_err(failed)?;
        self.position += frames as u64;
        self.save_checkpoint(false)
    }
}
