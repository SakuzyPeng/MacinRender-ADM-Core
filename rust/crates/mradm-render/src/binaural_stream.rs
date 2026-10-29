//! Binaural file streaming (PCM consistency 4.7 C), moved from the C++ `BinauralStream`: blocks of
//! the preparation's size go through the same per-source OLA bank as the offline renderer into a
//! FIFO served at any frame count, so a gap-free run from frame 0 is bit-identical to the offline
//! render. Live gain follows one 20 ms ramp per `object\0label` key; live topology scales rebuild
//! the source graph at most every 100 ms and crossfade the outgoing and incoming graphs over one
//! block. The bank and its workers live as long as the stream: a rebuild reuses their storage and
//! keeps the old graph when it fails. Seeks pre-roll on the offline window's block grid.
use crate::binaural::{self, Scales, Source, Spread};
use crate::binaural_render::{Bank, Offline, Prepared, preroll_start};
use crate::offline::{Error, Location, Result};
use crate::routing::{self, LiveOverride, Override};
use mradm_dsp::gain::GainBank;
use mradm_dsp::scene_math::Rotation;
use mradm_dsp::stream_dsp;
use mradm_wav::Reader;
use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;

/// The live gain de-zipper (`LiveGainRamp::k_default_ramp_ms`).
const RAMP_MS: u32 = 20;
/// The shortest interval between two live topology rebuilds.
const TOPOLOGY_INTERVAL_MS: u64 = 100;

/// The plan's input and controls.
pub struct Settings {
    pub inputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub smoothing_frames: u32,
    /// The plan's spread mode (the preparation's groups decide whether it streams).
    pub spread: Spread,
}

/// An override the stream keeps after `set_overrides` returns.
struct Owned {
    object_id: Vec<u8>,
    speaker_label: Vec<u8>,
    mute: bool,
    gain_db: f32,
    head_locked: Option<bool>,
}

impl Owned {
    fn view(&self) -> Override<'_> {
        Override {
            object_id: &self.object_id,
            speaker_label: &self.speaker_label,
            mute: self.mute,
            gain_db: self.gain_db,
            head_locked: self.head_locked,
        }
    }
}

/// One semantic gain key's ramp and the envelope of the current block.
struct Slot {
    object_id: Vec<u8>,
    label_key: Vec<u8>,
    ramp: GainBank,
    envelope: Vec<f32>,
}

fn failed(e: &mradm_dsp::Error) -> Error {
    Error::dsp(e, Location::None)
}

pub struct FileStream {
    prepared: Arc<Prepared>,
    reader: Reader<BufReader<File>>,
    /// The current source graph (the prepared list until a topology edit) and its OLA state.
    sources: Vec<Source>,
    bank: Bank,
    inputs: usize,
    frames: u64,
    sample_rate: u32,
    smoothing: u32,
    block: u64,
    input: Vec<f32>,
    head_locked: Vec<Option<bool>>,
    overrides: Vec<Owned>,
    pose: [f32; 3],
    slot_by_key: HashMap<Vec<u8>, usize>,
    slots: Vec<Slot>,
    /// Source index → `slots` index.
    source_slots: Vec<usize>,
    /// object_id → non-unity topology scales: the graph rendered and the newest request.
    applied: BTreeMap<Vec<u8>, Scales>,
    pending: BTreeMap<Vec<u8>, Scales>,
    topology_pending: bool,
    interval: u64,
    next_update: u64,
    /// Interleaved L/R produced but not yet served; one outgoing block during a crossfade.
    fifo: Vec<f32>,
    old_fifo: Vec<f32>,
    fifo_read: usize,
    done: u64,
    seek_target: u64,
}

impl FileStream {
    pub fn open(prepared: Arc<Prepared>, path: &str, s: Settings) -> Result<Self> {
        if s.spread == Spread::SafSpreader && !prepared.groups.is_empty() {
            return Err(Error {
                code: 2,
                message: "binaural realtime stream does not yet support saf_spreader mode (use \
                          cloud / none)"
                    .to_owned(),
                location: Location::None,
            });
        }
        if s.sample_rate != 48000 {
            return Err(Error {
                code: 1,
                message: format!(
                    "binaural renderer requires 48000 Hz input, got {}",
                    s.sample_rate
                ),
                location: Location::Input,
            });
        }
        let reader = Offline::open_reader(path)?;
        // One worker per core for the whole stream when the preparation has sources.
        let hardware = std::thread::available_parallelism().map_or(1, usize::from);
        let threads = if !prepared.sources.is_empty() && hardware > 1 {
            hardware
        } else {
            0
        };
        let block = prepared.block;
        let sources = prepared.sources.clone();
        let bank = Bank::new(&prepared.hrtf, sources.len(), block as usize, threads)?;
        let mut stream = Self {
            reader,
            sources,
            bank,
            inputs: s.inputs,
            frames: s.frames,
            sample_rate: s.sample_rate,
            smoothing: s.smoothing_frames,
            block,
            input: vec![0.; s.inputs * block as usize],
            head_locked: Vec::new(),
            overrides: Vec::new(),
            pose: [0.; 3],
            slot_by_key: HashMap::new(),
            slots: Vec::new(),
            source_slots: Vec::new(),
            applied: BTreeMap::new(),
            pending: BTreeMap::new(),
            topology_pending: false,
            interval: (u64::from(s.sample_rate) * TOPOLOGY_INTERVAL_MS / 1000).max(1),
            next_update: 0,
            fifo: Vec::new(),
            old_fifo: Vec::new(),
            fifo_read: 0,
            done: 0,
            seek_target: 0,
            prepared,
        };
        stream.assign_slots()?;
        Ok(stream)
    }

    /// Fills `output` (interleaved stereo) with up to `frames` frames; fewer only at the end.
    pub fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize> {
        if output.len() < frames.saturating_mul(2) {
            return Err(failed(&mradm_dsp::Error::InvalidArgument(
                "Invalid stream output buffer",
            )));
        }
        let mut produced = 0;
        while produced < frames {
            if self.fifo_read >= self.fifo.len() {
                if self.done >= self.frames {
                    break;
                }
                self.render_block()?;
                if self.fifo_read >= self.fifo.len() {
                    break;
                }
            }
            let take = (frames - produced).min((self.fifo.len() - self.fifo_read) / 2);
            output[produced * 2..][..take * 2]
                .copy_from_slice(&self.fifo[self.fifo_read..self.fifo_read + take * 2]);
            self.fifo_read += take * 2;
            produced += take;
        }
        Ok(produced)
    }

    /// Positions the input at the pre-roll start of `frame` and adopts the latest edits; true
    /// while `seek_step` still has blocks to render before the target.
    pub fn seek_begin(&mut self, frame: u64) -> Result<bool> {
        let target = frame.min(self.frames);
        let start = preroll_start(target, self.block, self.prepared.hrtf.overlap as u64);
        self.reader.seek_frame(start).map_err(Error::wav)?;
        self.done = start;
        self.fifo.clear();
        self.old_fifo.clear();
        self.fifo_read = 0;
        // A discontinuity has no outgoing graph to crossfade: adopt the latest edits before the
        // pre-roll, including targets still waiting for the topology interval.
        if self.topology_pending {
            let pending = std::mem::take(&mut self.pending);
            let rebuilt = self.rebuild(&pending);
            self.pending = pending;
            rebuilt?;
            self.applied.clone_from(&self.pending);
            self.topology_pending = false;
        } else {
            self.bank.reset();
        }
        self.next_update = target;
        for slot in &mut self.slots {
            slot.ramp.reset();
        }
        self.update_targets()?;
        self.seek_target = target;
        Ok(self.done < target)
    }

    /// Renders one pre-roll block, keeping the suffix of the block that holds the target so
    /// arbitrary pulls never shift the DSP grid.
    pub fn seek_step(&mut self) -> Result<bool> {
        let target = self.seek_target;
        if self.done >= target {
            return Ok(false);
        }
        let block_start = self.done;
        self.render_block()?;
        self.fifo_read = ((target - block_start).min(self.done - block_start) * 2) as usize;
        Ok(self.done < target)
    }

    /// Live gain targets per semantic key and the newest topology request. A topology change
    /// rebuilds the source graph at a block boundary, at most every 100 ms.
    pub fn set_overrides(&mut self, overrides: &[LiveOverride]) -> Result<()> {
        self.overrides.clear();
        self.overrides.extend(overrides.iter().map(|o| Owned {
            object_id: o.base.object_id.to_vec(),
            speaker_label: o.base.speaker_label.to_vec(),
            mute: o.base.mute,
            gain_db: o.base.gain_db,
            head_locked: o.base.head_locked,
        }));
        self.update_targets()?;
        let mut topology = BTreeMap::new();
        for o in overrides {
            let scales = [
                o.diffuse_scale,
                o.extent_scale * o.extent_width_scale,
                o.extent_scale * o.extent_height_scale,
                o.extent_scale * o.extent_depth_scale,
                o.divergence_scale,
            ];
            if scales.iter().any(|&s| s != 1.) {
                topology.insert(o.base.object_id.to_vec(), scales);
            }
        }
        self.pending = topology;
        self.topology_pending = self.pending != self.applied;
        Ok(())
    }

    /// The live listener orientation (yaw, pitch, roll); world-locked sources rotate into the
    /// head frame before the HRTF lookup.
    pub fn set_pose(&mut self, pose: [f32; 3]) {
        self.pose = pose;
    }

    fn update_targets(&mut self) -> Result<()> {
        for slot in &mut self.slots {
            let target = routing::try_live_override(
                self.overrides
                    .iter()
                    .map(|o| Ok::<_, std::convert::Infallible>(o.view())),
                &slot.object_id,
                &slot.label_key,
                true,
                false,
            )
            .map_or(None, |(gain, _)| gain)
            .unwrap_or(1.);
            slot.ramp.set_targets(&[target]).map_err(|e| failed(&e))?;
        }
        Ok(())
    }

    /// Binds each source to the slot of its key; keys keep their slot for the stream's lifetime.
    fn assign_slots(&mut self) -> Result<()> {
        self.source_slots.clear();
        for source in &self.sources {
            let mut key = source.object_id.clone();
            key.push(0);
            key.extend_from_slice(&source.label_key);
            let next = self.slots.len();
            let slot = *self.slot_by_key.entry(key).or_insert(next);
            if slot == next {
                self.slots.push(Slot {
                    object_id: source.object_id.clone(),
                    label_key: source.label_key.clone(),
                    ramp: GainBank::new(1, self.sample_rate, RAMP_MS).map_err(|e| failed(&e))?,
                    envelope: vec![1.; self.block as usize],
                });
            }
            self.source_slots.push(slot);
        }
        self.update_targets()
    }

    /// The scene's sources with each listed object's blocks scaled (none: the prepared list). The
    /// bank grows or shrinks first, so a failure keeps the old graph and its history.
    fn rebuild(&mut self, topology: &BTreeMap<Vec<u8>, Scales>) -> Result<()> {
        let sources = if topology.is_empty() {
            self.prepared.sources.clone()
        } else {
            let p = &self.prepared;
            binaural::sources(&p.input, p.spread, topology, &mut |_, _| {})
        };
        self.bank.rebuild(sources.len())?;
        self.sources = sources;
        self.assign_slots()
    }

    fn render_current(&mut self, old: bool, frames: usize) -> Result<()> {
        let head = if self.pose == [0.; 3] {
            None
        } else {
            Some(Rotation::new_compat(self.pose, false).map_err(|e| Error {
                code: 1,
                message: e.to_string(),
                location: Location::None,
            })?)
        };
        self.head_locked.clear();
        for source in &self.sources {
            let (_, locked) = routing::try_live_override(
                self.overrides
                    .iter()
                    .map(|o| Ok::<_, std::convert::Infallible>(o.view())),
                &source.object_id,
                &source.label_key,
                false,
                true,
            )
            .unwrap_or((None, None));
            self.head_locked.push(locked);
        }
        let locked = &self.head_locked;
        self.bank.render(
            &self.sources,
            &self.prepared.hrtf.filters,
            &self.input,
            self.inputs,
            self.done,
            frames,
            self.smoothing,
            head.as_ref(),
            &|i| locked[i],
        )?;
        // Reduce per-source L/R in source order with each key's envelope: without overrides
        // every gain is 1 and the sum matches the offline reduction.
        let output = if old {
            &mut self.old_fifo
        } else {
            &mut self.fifo
        };
        output.clear();
        output.resize(frames * 2, 0.);
        for (state, &slot) in self.bank.states.iter().zip(&self.source_slots) {
            stream_dsp::accumulate_stereo(
                output,
                &state.left,
                &state.right,
                &self.slots[slot].envelope,
                frames,
            )
            .map_err(|e| failed(&e))?;
        }
        Ok(())
    }

    fn render_block(&mut self) -> Result<()> {
        let frames = self.block.min(self.frames - self.done) as usize;
        let read = self
            .reader
            .read_frames(&mut self.input[..frames * self.inputs])
            .map_err(Error::wav)?;
        if read != frames as u64 {
            return Err(Error {
                code: 3,
                message: "short render input read".to_owned(),
                location: Location::None,
            });
        }
        // Before the first sample there is no outgoing state to keep: apply the coalesced
        // initial topology directly, then start the envelopes on the final graph.
        if self.done == 0 && self.topology_pending {
            self.adopt_pending()?;
        }
        for slot in &mut self.slots {
            slot.ramp
                .fill(&mut slot.envelope[..frames])
                .map_err(|e| failed(&e))?;
        }
        if self.topology_pending && self.done >= self.next_update {
            self.render_current(true, frames)?;
            self.adopt_pending()?;
            self.render_current(false, frames)?;
            stream_dsp::topology_crossfade(&self.old_fifo, &mut self.fifo, frames)
                .map_err(|e| failed(&e))?;
            self.next_update = self.done + self.interval;
        } else {
            self.render_current(false, frames)?;
        }
        self.fifo_read = 0;
        self.done += frames as u64;
        Ok(())
    }

    fn adopt_pending(&mut self) -> Result<()> {
        let pending = std::mem::take(&mut self.pending);
        let rebuilt = self.rebuild(&pending);
        self.pending = pending;
        rebuilt?;
        self.applied.clone_from(&self.pending);
        self.topology_pending = false;
        Ok(())
    }
}
