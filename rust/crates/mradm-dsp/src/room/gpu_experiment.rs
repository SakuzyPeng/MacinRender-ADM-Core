//! Research-only batch boundary. The Session still owns control, EQ, sends,
//! late field, LFE, head-locked processing, FIFO, switching and tail accounting.
//! No unsafe code or native GPU dependency belongs to this module.
mod reference;
use super::{
    BLOCK,
    geometry::{self, Rotation, WeightedPosition},
    hrir::PHASES,
    partition::InputHistory,
    prepared::PreparedRoom,
};
use crate::{Error, Result};
pub use reference::ReferenceFactory;
use std::sync::Arc;

pub const MAX_VOICES: usize = 32;
pub const SLOTS: usize = 32;
pub const MAX_ACTIVE: usize = 16;
const JOB_LIMIT: usize = 8192;
const TARGET_JOB_LIMIT: usize = 4096;
const UPDATE_LIMIT: usize = 32;

pub trait Factory: Send + Sync {
    fn create(&self, room: Arc<PreparedRoom>) -> Result<Box<dyn Backend>>;
    fn create_late(&self, room: Arc<PreparedRoom>) -> Result<Option<Box<dyn LateBackend>>>;
}
/// The 2048-frame late partition is computed after its input closes, then
/// consumed starting at the next canonical block, just like LongReader.
pub trait LateBackend: Send {
    fn submit(&mut self, frequency: &[crate::Complex32], reset: bool) -> Result<()>;
    fn finish(&mut self, output: &mut [f32]) -> Result<()>;
}
pub trait Backend: Send {
    fn begin(&mut self, voices: usize) -> Result<()>;
    fn voice(&mut self, index: usize, frame: &Frame) -> Result<()>;
    fn submit(&mut self) -> Result<()>;
    fn finish(&mut self, output: &mut [f32]) -> Result<()>;
}
#[derive(Clone, Copy, Default, PartialEq)]
#[repr(C)]
pub struct Term {
    pub row: u32,
    pub fraction: u32,
    pub shift: u32,
    pub alpha: f32,
    pub weight: f32,
}
#[derive(Clone, Copy, Default, PartialEq)]
#[repr(C)]
pub struct Job {
    pub material: u32,
    pub fraction: u32,
    pub shift: u32,
    pub counts: u32,
    pub alpha: f32,
    pub gain: f32,
    pub terms: [Term; 12],
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Range {
    pub first: u32,
    pub count: u32,
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Update {
    pub slot: u32,
    pub branch: u32,
    pub range_first: u32,
    pub range_count: u32,
    pub parts: u32,
    pub support: u32,
}
pub struct Request {
    pub update: Update,
    pub sources: std::ops::Range<usize>,
    pub rotation: Rotation,
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Task {
    pub start: u64,
    pub end: u64,
    pub block_start: u64,
    pub slot: u32,
    pub branch: u32,
    pub parts: u32,
    pub begin: u32,
    pub frames: u32,
    pub first: u32,
}
impl Task {
    pub fn weight(&self, index: usize, fade: u64) -> f32 {
        if index < self.begin as usize || index >= (self.begin + self.frames) as usize {
            return 0.;
        }
        let sample = self.block_start + index as u64;
        let right = sample.min(self.end);
        let duration = if self.first != 0 {
            fade.saturating_sub(sample - right)
        } else {
            right.saturating_sub(sample.saturating_sub(fade).max(self.start))
        };
        duration as f32 / fade as f32
    }
}
pub struct Frame {
    pub jobs: Vec<Job>,
    pub ranges: Vec<Range>,
    pub requests: Vec<Request>,
    pub tasks: Vec<Task>,
    pub sources: Vec<WeightedPosition>,
    pub frequency: [f32; 4 * (BLOCK + 1)],
    pub heads: [u32; 2],
    pub active: bool,
    pub reset: bool,
}
impl Frame {
    fn new(buckets: usize) -> Self {
        Self {
            jobs: Vec::with_capacity(JOB_LIMIT),
            ranges: Vec::with_capacity(UPDATE_LIMIT * buckets),
            requests: Vec::with_capacity(UPDATE_LIMIT),
            tasks: Vec::with_capacity(2 * SLOTS),
            sources: Vec::with_capacity(UPDATE_LIMIT * super::spatial::MAX_SAMPLES),
            frequency: [0.; 4 * (BLOCK + 1)],
            heads: [0; 2],
            active: false,
            reset: true,
        }
    }
    fn clear(&mut self) {
        self.jobs.clear();
        self.ranges.clear();
        self.requests.clear();
        self.tasks.clear();
        self.sources.clear();
        self.active = false;
    }
}
#[derive(Clone, Copy)]
struct Target {
    start: u64,
    end: u64,
    support: usize,
}
struct Timeline {
    targets: [Target; SLOTS],
    order: [usize; MAX_ACTIVE],
    count: usize,
    used: [bool; SLOTS],
    tasks: [usize; SLOTS],
    updates: [usize; SLOTS],
    last_jobs: Vec<Job>,
    last_counts: Vec<u32>,
    last_support: usize,
    valid: bool,
}
impl Timeline {
    fn new(buckets: usize) -> Self {
        Self {
            targets: [Target {
                start: 0,
                end: u64::MAX,
                support: 1,
            }; SLOTS],
            order: [0; MAX_ACTIVE],
            count: 0,
            used: [false; SLOTS],
            tasks: [usize::MAX; SLOTS],
            updates: [usize::MAX; SLOTS],
            last_jobs: Vec::with_capacity(TARGET_JOB_LIMIT),
            last_counts: vec![0; buckets],
            last_support: 0,
            valid: false,
        }
    }
    fn reset(&mut self) {
        self.count = 0;
        self.valid = false;
        self.begin();
    }
    fn begin(&mut self) {
        self.used.fill(false);
        self.tasks.fill(usize::MAX);
        self.updates.fill(usize::MAX);
        for slot in &self.order[..self.count] {
            self.used[*slot] = true;
        }
    }
    fn prune(&mut self, sample: u64, fade: u64) {
        if sample < fade {
            return;
        }
        while self.count > 1 && self.targets[self.order[1]].start <= sample - fade {
            self.order.copy_within(1..self.count, 0);
            self.count -= 1;
        }
    }
    fn target(
        &mut self,
        sample: u64,
        support: usize,
        fade: u64,
        frame: &mut Frame,
    ) -> Result<usize> {
        self.prune(sample, fade);
        if self.count > 1 && self.targets[self.order[self.count - 1]].start == sample {
            let slot = self.order[self.count - 1];
            self.targets[slot].support = support;
            return Ok(slot);
        }
        if self.count == MAX_ACTIVE {
            return Err(Error::Unsupported(
                "Room control exceeds 16 distinct targets per 10 ms",
            ));
        }
        let slot = self
            .used
            .iter()
            .position(|v| !*v)
            .ok_or(Error::Unsupported("GPU frame target storage exhausted"))?;
        if self.count != 0 {
            let previous = self.order[self.count - 1];
            self.targets[previous].end = sample;
            if self.tasks[previous] != usize::MAX {
                frame.tasks[self.tasks[previous]].end = sample;
            }
        }
        self.used[slot] = true;
        self.targets[slot] = Target {
            start: sample,
            end: u64::MAX,
            support,
        };
        self.order[self.count] = slot;
        self.count += 1;
        Ok(slot)
    }
    fn render(
        &mut self,
        branch: usize,
        start: u64,
        offset: usize,
        frames: usize,
        fade: u64,
        out: &mut Frame,
    ) -> Result<()> {
        self.prune(start + offset as u64, fade);
        for (j, slot) in self.order[..self.count].iter().copied().enumerate() {
            if self.tasks[slot] == usize::MAX {
                if out.tasks.len() == out.tasks.capacity() {
                    return Err(Error::Unsupported("GPU frame task capacity exceeded"));
                }
                let target = self.targets[slot];
                self.tasks[slot] = out.tasks.len();
                out.tasks.push(Task {
                    start: target.start,
                    end: target.end,
                    block_start: start,
                    slot: slot as u32,
                    branch: branch as u32,
                    parts: target.support.div_ceil(BLOCK) as u32,
                    begin: offset as u32,
                    frames: frames as u32,
                    first: u32::from(j == 0),
                });
            } else {
                let task = &mut out.tasks[self.tasks[slot]];
                if task.begin + task.frames != offset as u32 {
                    return Err(Error::Unsupported("Noncontiguous GPU voice rendering"));
                }
                task.frames += frames as u32;
            }
        }
        self.prune(start + (offset + frames) as u64, fade);
        Ok(())
    }
}
pub(crate) struct Voice {
    room: Arc<PreparedRoom>,
    timelines: [Timeline; 2],
    scratch: Vec<(usize, usize, Job)>,
    counts: Vec<u32>,
    faces: Vec<usize>,
    pub frame: Frame,
    need_reset: bool,
}
fn fraction(value: f64) -> (u32, f32) {
    let x = value.clamp(0., 1.) * PHASES as f64;
    let low = (x as usize).min(PHASES - 1);
    (low as u32, (x - low as f64) as f32)
}
impl Voice {
    pub fn new(room: Arc<PreparedRoom>) -> Result<Self> {
        if room.quality != super::quality::Quality::Realtime || room.early_channels() != 2 {
            return Err(Error::Unsupported(
                "GPU experiment requires realtime binaural early paths",
            ));
        }
        let buckets = room.short_taps.div_ceil(room.path_bucket_stride);
        Ok(Self {
            timelines: std::array::from_fn(|_| Timeline::new(buckets)),
            scratch: Vec::with_capacity(TARGET_JOB_LIMIT),
            counts: vec![0; buckets],
            faces: vec![usize::MAX; room.images.len()],
            frame: Frame::new(buckets),
            room,
            need_reset: true,
        })
    }
    pub fn reset(&mut self) {
        for t in &mut self.timelines {
            t.reset();
        }
        self.need_reset = true;
        self.frame.clear();
        self.frame.reset = true;
    }
    pub fn begin(&mut self) {
        self.frame.clear();
        self.frame.reset = self.need_reset;
        self.need_reset = false;
        for t in &mut self.timelines {
            t.begin();
        }
    }
    pub fn inputs(&mut self, direct: &InputHistory, wet: &InputHistory) {
        self.frame.reset |= self.need_reset;
        self.need_reset = false;
        self.frame.active = true;
        for (branch, history) in [direct, wet].into_iter().enumerate() {
            let (head, row) = history.gpu_latest();
            self.frame.heads[branch] = head as u32;
            for (bin, value) in row.iter().enumerate() {
                let at = (branch * (BLOCK + 1) + bin) * 2;
                self.frame.frequency[at] = value.re;
                self.frame.frequency[at + 1] = value.im;
            }
        }
    }
    pub fn render(
        &mut self,
        branch: usize,
        block_start: u64,
        offset: usize,
        frames: usize,
    ) -> Result<()> {
        self.timelines[branch].render(
            branch,
            block_start,
            offset,
            frames,
            super::filter_fade(self.room.profile.sample_rate) as u64,
            &mut self.frame,
        )
    }
    pub fn target(
        &mut self,
        branch: usize,
        clock: u64,
        sources: &[WeightedPosition],
        rotation: Rotation,
    ) -> Result<()> {
        self.scratch.clear();
        self.counts.fill(0);
        let bank = &self.room.hrir;
        let data = &bank.data;
        let n = self.room.fft_length;
        let images = if branch == 0 {
            0..1
        } else {
            1..self.room.images.len()
        };
        let mut support = 1;
        for source in sources {
            for index in images.clone() {
                let image = self.room.images[index];
                let path = geometry::path(&self.room.profile, source.position, image);
                if path.gain == 0. {
                    continue;
                }
                if self.scratch.len() == TARGET_JOB_LIMIT {
                    return Err(Error::Unsupported("GPU target path capacity exceeded"));
                }
                let absolute =
                    libm::floor(path.samples) as usize + self.room.profile.alignment_guard;
                support = support.max(
                    absolute
                        + data.output_taps
                        + super::hrir::FRACTIONAL_TAPS
                        + self.room.profile.band_taps
                        - 2,
                );
                let (pf, pa) =
                    fraction(f64::from((path.samples - libm::floor(path.samples)) as f32));
                let mut job = Job {
                    material: index as u32,
                    fraction: pf,
                    shift: (absolute % self.room.path_bucket_stride) as u32,
                    alpha: pa,
                    gain: path.gain as f32 * source.weight,
                    ..Job::default()
                };
                let (vertices, weights) =
                    bank.weights(rotation.apply(path.direction), &mut self.faces[index])?;
                let radius = if branch == 0 {
                    source.position.distance
                } else {
                    path.distance
                };
                let selected = if let Some(collection) = &data.collection {
                    let (lo, hi, a) = collection.bracket(radius);
                    [(lo, 1. - a), (hi, if lo == hi { 0. } else { a })]
                } else {
                    [(0, 1.), (0, 0.)]
                };
                for ear in 0..2 {
                    let mut delay = 0.;
                    for (layer, radial) in selected {
                        if radial == 0. {
                            continue;
                        }
                        let d = if data.collection.is_some() {
                            data.layer(layer)
                        } else {
                            data
                        };
                        for v in 0..3 {
                            delay += d.delays[vertices[v]][ear] * (weights[v] * radial);
                        }
                    }
                    let mut count = 0;
                    for (layer, radial) in selected {
                        if radial == 0. {
                            continue;
                        }
                        let d = if data.collection.is_some() {
                            data.layer(layer)
                        } else {
                            data
                        };
                        for v in 0..3 {
                            let weight = weights[v] * radial;
                            if weight <= 0. {
                                continue;
                            }
                            let shift = delay - d.delays[vertices[v]][ear];
                            let integer = libm::floor(shift);
                            let (f, a) = fraction(shift - integer);
                            job.terms[ear * 6 + count] = Term {
                                row: ((layer * data.directions.len() + vertices[v]) * 2 + ear)
                                    as u32,
                                fraction: f,
                                shift: (data.guard as i64 + integer as i64 - 40)
                                    .rem_euclid(n as i64)
                                    as u32,
                                alpha: a,
                                weight: weight as f32,
                            };
                            count += 1;
                        }
                    }
                    job.counts |= (count as u32) << (ear * 8);
                }
                let bucket = absolute / self.room.path_bucket_stride;
                self.counts[bucket] += 1;
                self.scratch.push((bucket, self.scratch.len(), job));
            }
        }
        self.scratch.sort_unstable_by_key(|v| (v.0, v.1));
        let timeline = &mut self.timelines[branch];
        if timeline.valid
            && support == timeline.last_support
            && self.counts == timeline.last_counts
            && timeline.last_jobs.len() == self.scratch.len()
            && timeline
                .last_jobs
                .iter()
                .zip(&self.scratch)
                .all(|(a, b)| *a == b.2)
        {
            return Ok(());
        }
        if self.frame.jobs.len() + self.scratch.len() > JOB_LIMIT
            || self.frame.requests.len() == UPDATE_LIMIT
        {
            return Err(Error::Unsupported("GPU frame update capacity exceeded"));
        }
        let slot = timeline.target(
            clock,
            support,
            super::filter_fade(self.room.profile.sample_rate) as u64,
            &mut self.frame,
        )?;
        let range_first = self.frame.ranges.len();
        let mut first = self.frame.jobs.len();
        for count in &self.counts {
            self.frame.ranges.push(Range {
                first: first as u32,
                count: *count,
            });
            first += *count as usize;
        }
        self.frame.jobs.extend(self.scratch.iter().map(|v| v.2));
        let source_first = self.frame.sources.len();
        self.frame.sources.extend_from_slice(sources);
        let request = Request {
            update: Update {
                slot: slot as u32,
                branch: branch as u32,
                range_first: range_first as u32,
                range_count: self.counts.len() as u32,
                parts: support.div_ceil(BLOCK) as u32,
                support: support as u32,
            },
            sources: source_first..self.frame.sources.len(),
            rotation,
        };
        if timeline.updates[slot] != usize::MAX {
            self.frame.requests[timeline.updates[slot]] = request;
        } else {
            timeline.updates[slot] = self.frame.requests.len();
            self.frame.requests.push(request);
        }
        timeline.last_jobs.clear();
        timeline.last_jobs.extend(self.scratch.iter().map(|v| v.2));
        timeline.last_counts.copy_from_slice(&self.counts);
        timeline.last_support = support;
        timeline.valid = true;
        Ok(())
    }
}
