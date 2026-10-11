//! Finite partitioned convolution with explicit sample support. Filter changes
//! use the same unfiltered input history; an outgoing output tail is never copied
//! into a new filter. No processing method allocates.
use crate::{Complex32, Error, Result, fft::RealFft};
use std::sync::Arc;

#[derive(Clone)]
pub struct Filters {
    pub block: usize,
    pub channels: usize,
    pub taps: usize,
    partitions: usize,
    used_partitions: usize,
    support: usize,
    spectrum: Vec<Complex32>,
}

impl Filters {
    pub fn empty(block: usize, channels: usize, taps: usize) -> Result<Self> {
        if !block.is_power_of_two()
            || !(16..=4096).contains(&block)
            || !(1..=64).contains(&channels)
            || taps == 0
            || taps > 1 << 20
        {
            return Err(Error::InvalidArgument(
                "Invalid room convolution dimensions",
            ));
        }
        let partitions = taps.div_ceil(block);
        let length = channels
            .checked_mul(partitions)
            .and_then(|n| n.checked_mul(block + 1))
            .filter(|n| *n <= (1 << 26))
            .ok_or(Error::InvalidArgument("Room convolution is too large"))?;
        Ok(Self {
            block,
            channels,
            taps,
            partitions,
            used_partitions: partitions,
            support: taps,
            spectrum: vec![Complex32::default(); length],
        })
    }

    /// Channel-major FIRs, each exactly `taps` long.
    pub fn prepare(block: usize, channels: usize, impulse: &[f32]) -> Result<Self> {
        if channels == 0 || !impulse.len().is_multiple_of(channels) {
            return Err(Error::InvalidArgument("Invalid room FIR channels"));
        }
        let mut result = Self::empty(block, channels, impulse.len() / channels)?;
        let mut work = Uniform::new(&result)?;
        work.write_filters(&mut result, impulse)?;
        Ok(result)
    }
}

pub struct Uniform {
    block: usize,
    partitions: usize,
    fft: RealFft,
    previous: Vec<f32>,
    time: Vec<f32>,
    ring: Vec<Complex32>,
    sum: Vec<Complex32>,
    head: usize,
}

impl Uniform {
    pub fn new(filters: &Filters) -> Result<Self> {
        Self::workspace(filters, true)
    }
    fn workspace(filters: &Filters, history: bool) -> Result<Self> {
        let block = filters.block;
        let mut result = Self {
            block,
            partitions: filters.partitions,
            fft: RealFft::new(block * 2)?,
            previous: vec![0.; if history { block } else { 0 }],
            time: vec![0.; block * 2],
            ring: vec![
                Complex32::default();
                if history {
                    filters.partitions * (block + 1)
                } else {
                    0
                }
            ],
            sum: vec![Complex32::default(); block + 1],
            head: filters.partitions - 1,
        };
        super::touch_pages(&mut result.previous);
        super::touch_pages(&mut result.time);
        super::touch_pages(&mut result.ring);
        super::touch_pages(&mut result.sum);
        Ok(result)
    }

    pub fn reset(&mut self) {
        self.previous.fill(0.);
        self.ring.fill(Complex32::default());
        self.head = self.partitions - 1;
    }

    fn compatible(&self, filters: &Filters) -> Result<()> {
        if self.block != filters.block || self.partitions != filters.partitions {
            return Err(Error::InvalidArgument("Incompatible room partition state"));
        }
        Ok(())
    }

    pub fn write_filters(&mut self, filters: &mut Filters, impulse: &[f32]) -> Result<()> {
        self.write_filter_support(filters, impulse, filters.taps)
    }
    fn write_filter_support(
        &mut self,
        filters: &mut Filters,
        impulse: &[f32],
        support: usize,
    ) -> Result<()> {
        self.compatible(filters)?;
        if support == 0
            || support > filters.taps
            || impulse.len() != filters.taps * filters.channels
            || impulse.iter().any(|v| !v.is_finite())
        {
            return Err(Error::InvalidArgument("Invalid room impulse response"));
        }
        self.write_validated_filter_support(filters, impulse, support)
    }
    // Both callers validate the complete impulse, including values outside its
    // structural support. Dynamic owns compatible filter slots and performs this
    // validation before pruning or selecting a target; do not scan it again.
    fn write_validated_filter_support(
        &mut self,
        filters: &mut Filters,
        impulse: &[f32],
        support: usize,
    ) -> Result<()> {
        let bins = self.block + 1;
        filters.used_partitions = support.div_ceil(self.block);
        filters.support = support;
        for channel in 0..filters.channels {
            for part in 0..filters.used_partitions {
                let begin = part * self.block;
                let count = self.block.min(support - begin);
                let offset = (channel * self.partitions + part) * bins;
                self.fft.forward_padded(
                    &impulse
                        [channel * filters.taps + begin..channel * filters.taps + begin + count],
                    &mut filters.spectrum[offset..offset + bins],
                )?;
            }
        }
        Ok(())
    }

    pub fn push(&mut self, input: &[f32]) -> Result<()> {
        if self.previous.len() != self.block
            || input.len() != self.block
            || input.iter().any(|v| !v.is_finite())
        {
            return Err(Error::InvalidArgument("Invalid room convolution input"));
        }
        self.time[..self.block].copy_from_slice(&self.previous);
        self.time[self.block..].copy_from_slice(input);
        self.previous.copy_from_slice(input);
        self.head = (self.head + 1) % self.partitions;
        let begin = self.head * (self.block + 1);
        self.fft
            .forward(&self.time, &mut self.ring[begin..begin + self.block + 1])
    }

    /// Render the most recently pushed block, overwriting channel-major output.
    pub fn render(&mut self, filters: &Filters, output: &mut [f32]) -> Result<()> {
        self.compatible(filters)?;
        Self::render_parts(
            &mut self.fft,
            &mut self.sum,
            &mut self.time,
            &self.ring,
            self.head,
            self.partitions,
            filters,
            output,
        )
    }
    fn render_shared(
        &mut self,
        filters: &Filters,
        history: &Self,
        output: &mut [f32],
    ) -> Result<()> {
        self.compatible(filters)?;
        if history.block != filters.block || history.partitions < filters.partitions {
            return Err(Error::InvalidArgument(
                "Insufficient shared room input history",
            ));
        }
        Self::render_parts(
            &mut self.fft,
            &mut self.sum,
            &mut self.time,
            &history.ring,
            history.head,
            history.partitions,
            filters,
            output,
        )
    }
    fn render_parts(
        fft: &mut RealFft,
        sum: &mut [Complex32],
        time: &mut [f32],
        ring: &[Complex32],
        head: usize,
        partitions: usize,
        filters: &Filters,
        output: &mut [f32],
    ) -> Result<()> {
        let block = filters.block;
        if output.len() != filters.channels * block || ring.len() != partitions * (block + 1) {
            return Err(Error::InvalidArgument("Invalid room convolution output"));
        }
        let bins = block + 1;
        for (channel, destination) in output.chunks_exact_mut(block).enumerate() {
            sum.fill(Complex32::default());
            for part in 0..filters.used_partitions {
                let history = (head + partitions - part) % partitions;
                let x = &ring[history * bins..(history + 1) * bins];
                let begin = (channel * filters.partitions + part) * bins;
                let h = &filters.spectrum[begin..begin + bins];
                for ((sum, x), h) in sum.iter_mut().zip(x).zip(h) {
                    *sum += *x * *h;
                }
            }
            fft.inverse(sum, time)?;
            destination.copy_from_slice(&time[block..]);
        }
        Ok(())
    }

    pub fn copy_history_from(&mut self, source: &Self) -> Result<()> {
        if self.block != source.block
            || self.partitions != source.partitions
            || self.previous.len() != source.previous.len()
            || self.ring.len() != source.ring.len()
        {
            return Err(Error::InvalidArgument("Incompatible room input history"));
        }
        self.previous.copy_from_slice(&source.previous);
        self.ring.copy_from_slice(&source.ring);
        self.head = source.head;
        Ok(())
    }
}

const MAX_TARGETS: usize = 16;

/// One mono input FFT/history can feed several filters of different lengths.
/// The owner advances it exactly once per block, before rendering any reader.
pub struct InputHistory {
    ring: Vec<Complex32>,
    previous: [f32; super::BLOCK],
    head: usize,
    partitions: usize,
}
impl InputHistory {
    pub fn new(taps: usize) -> Result<Self> {
        if taps == 0 || taps > 1 << 20 {
            return Err(Error::InvalidArgument(
                "Invalid shared room history support",
            ));
        }
        let partitions = taps.div_ceil(super::BLOCK);
        let mut ring = vec![Complex32::default(); partitions * (super::BLOCK + 1)];
        super::touch_pages(&mut ring);
        Ok(Self {
            ring,
            previous: [0.; super::BLOCK],
            head: partitions - 1,
            partitions,
        })
    }
    pub fn reset(&mut self) {
        self.ring.fill(Complex32::default());
        self.previous.fill(0.);
        self.head = self.partitions - 1;
    }
    // Reuse a reader's existing FFT workspace instead of preparing another plan
    // per history. No filter or cached output state is changed here.
    fn push(&mut self, input: &[f32], work: &mut Uniform) -> Result<()> {
        let block = super::BLOCK;
        if work.block != block || input.len() != block || input.iter().any(|v| !v.is_finite()) {
            return Err(Error::InvalidArgument("Invalid shared room history input"));
        }
        work.time[..block].copy_from_slice(&self.previous);
        work.time[block..].copy_from_slice(input);
        self.previous.copy_from_slice(input);
        self.head = (self.head + 1) % self.partitions;
        let begin = self.head * (block + 1);
        work.fft
            .forward(&work.time, &mut self.ring[begin..begin + block + 1])
    }
    pub fn copy_history_from(&mut self, source: &Self) -> Result<()> {
        if self.partitions != source.partitions {
            return Err(Error::InvalidArgument(
                "Incompatible shared room input history",
            ));
        }
        self.ring.copy_from_slice(&source.ring);
        self.previous = source.previous;
        self.head = source.head;
        Ok(())
    }
}

struct Target {
    filter: Filters,
    start: u64,
    cache: Vec<f32>,
    cached: bool,
}

/// A 10 ms finite moving average of target filters. Each new target starts with
/// zero weight, preserving the audible response without recursively extending
/// its memory. A seek therefore needs only FIR history plus FILTER_FADE samples.
/// Up to 16 distinct targets per 10 ms are prepared; excess rate is an explicit
/// resource error, never a machine-dependent quality reduction.
pub struct Dynamic {
    core: Uniform,
    targets: Vec<Target>,
    order: [usize; MAX_TARGETS],
    count: usize,
    last_impulse: Vec<f32>,
    clock: u64,
    block_start: u64,
    cursor: usize,
}

impl Dynamic {
    pub fn new(channels: usize, taps: usize) -> Result<Self> {
        Self::workspace(channels, taps, true)
    }
    pub fn reader(channels: usize, taps: usize) -> Result<Self> {
        Self::workspace(channels, taps, false)
    }
    fn workspace(channels: usize, taps: usize, history: bool) -> Result<Self> {
        let filter = Filters::empty(super::BLOCK, channels, taps)?;
        let core = Uniform::workspace(&filter, history)?;
        let mut targets: Vec<Target> = (0..MAX_TARGETS)
            .map(|_| Target {
                filter: filter.clone(),
                start: 0,
                cache: vec![0.; channels * super::BLOCK],
                cached: false,
            })
            .collect();
        for target in &mut targets {
            super::touch_pages(&mut target.cache);
            super::touch_pages(&mut target.filter.spectrum);
        }
        Ok(Self {
            core,
            targets,
            order: [0; MAX_TARGETS],
            count: 0,
            last_impulse: vec![0.; channels * taps],
            clock: 0,
            block_start: 0,
            cursor: 0,
        })
    }
    pub fn reset(&mut self) {
        self.core.reset();
        self.count = 0;
        self.clock = 0;
        self.block_start = 0;
        self.cursor = 0;
        for target in &mut self.targets {
            target.cached = false;
        }
    }
    fn prune(&mut self, sample: u64) {
        if sample < super::FILTER_FADE as u64 {
            return;
        }
        let left = sample - super::FILTER_FADE as u64;
        while self.count > 1 && self.targets[self.order[1]].start <= left {
            self.order.copy_within(1..self.count, 0);
            self.count -= 1;
        }
    }
    pub fn set_target(&mut self, impulse: &[f32]) -> Result<()> {
        let taps = self.targets[0].filter.taps;
        self.set_target_support(impulse, taps)
    }
    /// The caller supplies a structural FIR support bound, never an energy
    /// threshold. Input history keeps the full maximum for subsequent targets.
    pub fn set_target_support(&mut self, impulse: &[f32], support: usize) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::TargetFilters);
        if impulse.len() != self.last_impulse.len() || impulse.iter().any(|v| !v.is_finite()) {
            return Err(Error::InvalidArgument(
                "Invalid room target impulse response",
            ));
        }
        if support == 0 || support > self.targets[0].filter.taps {
            return Err(Error::InvalidArgument("Invalid room FIR support"));
        }
        if self.count != 0
            && self.last_impulse == impulse
            && self.targets[self.order[self.count - 1]].filter.support == support
        {
            return Ok(());
        }
        self.prune(self.clock);
        // Several changes at one sample replace the inaudible newest target.
        let replace =
            self.count > 1 && self.targets[self.order[self.count - 1]].start == self.clock;
        let index = if replace {
            self.order[self.count - 1]
        } else {
            if self.count == MAX_TARGETS {
                return Err(Error::Unsupported(
                    "Room control exceeds 16 distinct targets per 10 ms",
                ));
            }
            let index = (0..MAX_TARGETS)
                .find(|i| !self.order[..self.count].contains(i))
                .expect("free prepared room target");
            self.order[self.count] = index;
            self.count += 1;
            index
        };
        let target = &mut self.targets[index];
        self.core
            .write_validated_filter_support(&mut target.filter, impulse, support)?;
        target.start = self.clock;
        target.cached = false;
        self.last_impulse.copy_from_slice(impulse);
        Ok(())
    }
    pub fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        self.begin(input)?;
        self.render_range(0, super::BLOCK, output)
    }
    pub fn begin(&mut self, input: &[f32]) -> Result<()> {
        self.core.push(input)?;
        self.begin_shared();
        Ok(())
    }
    pub fn begin_with_history(&mut self, history: &mut InputHistory, input: &[f32]) -> Result<()> {
        history.push(input, &mut self.core)?;
        self.begin_shared();
        Ok(())
    }
    pub fn begin_shared(&mut self) {
        self.block_start = self.clock;
        self.cursor = 0;
        for target in &mut self.targets {
            target.cached = false;
        }
    }
    /// Chronological subranges share one input FFT and immutable target output
    /// caches. Splitting a range cannot change the per-sample summation order.
    pub fn render_range(&mut self, offset: usize, frames: usize, output: &mut [f32]) -> Result<()> {
        self.render_with_history(None, offset, frames, output)
    }
    pub fn render_shared_range(
        &mut self,
        history: &InputHistory,
        offset: usize,
        frames: usize,
        output: &mut [f32],
    ) -> Result<()> {
        self.render_with_history(Some(history), offset, frames, output)
    }
    fn render_with_history(
        &mut self,
        history: Option<&InputHistory>,
        offset: usize,
        frames: usize,
        output: &mut [f32],
    ) -> Result<()> {
        let channels = self.targets[0].filter.channels;
        if offset < self.cursor
            || offset > super::BLOCK
            || frames > super::BLOCK - offset
            || output.len() != channels * super::BLOCK
        {
            return Err(Error::InvalidArgument("Invalid room convolution subrange"));
        }
        self.prune(self.block_start + offset as u64);
        for index in &self.order[..self.count] {
            let target = &mut self.targets[*index];
            if !target.cached {
                if let Some(history) = history {
                    if history.partitions < target.filter.partitions {
                        return Err(Error::InvalidArgument(
                            "Insufficient shared room input history",
                        ));
                    }
                    Uniform::render_parts(
                        &mut self.core.fft,
                        &mut self.core.sum,
                        &mut self.core.time,
                        &history.ring,
                        history.head,
                        history.partitions,
                        &target.filter,
                        &mut target.cache,
                    )?;
                } else {
                    self.core.render(&target.filter, &mut target.cache)?;
                }
                target.cached = true;
            }
        }
        for channel in 0..channels {
            output[channel * super::BLOCK + offset..channel * super::BLOCK + offset + frames]
                .fill(0.);
        }
        for j in 0..self.count {
            let target = &self.targets[self.order[j]];
            let end = if j + 1 < self.count {
                self.targets[self.order[j + 1]].start
            } else {
                u64::MAX
            };
            for i in offset..offset + frames {
                let sample = self.block_start + i as u64;
                let right = sample.min(end);
                let duration = if j == 0 {
                    (super::FILTER_FADE as u64).saturating_sub(sample - right)
                } else {
                    right.saturating_sub(
                        sample
                            .saturating_sub(super::FILTER_FADE as u64)
                            .max(target.start),
                    )
                };
                if duration == 0 {
                    continue;
                }
                let weight = duration as f32 / super::FILTER_FADE as f32;
                for channel in 0..channels {
                    let at = channel * super::BLOCK + i;
                    output[at] += target.cache[at] * weight;
                }
            }
        }
        self.cursor = offset + frames;
        self.clock = self.block_start + self.cursor as u64;
        self.prune(self.clock);
        Ok(())
    }
}

pub struct LongFilters {
    pub taps: usize,
    pub channels: usize,
    head: Filters,
    tail: Option<Filters>,
}

impl LongFilters {
    pub fn fingerprint(&self) -> serde_json::Value {
        serde_json::json!({"taps":self.taps,"channels":self.channels,
            "head":super::resources::hash_complex(&self.head.spectrum),
            "tail":self.tail.as_ref().map(|t|super::resources::hash_complex(&t.spectrum))})
    }
    pub fn history_frames(&self) -> usize {
        self.head.partitions * self.head.block
            + self.tail.as_ref().map_or(0, |tail| {
                (2048 + tail.partitions * tail.block)
                    .saturating_sub(self.head.partitions * self.head.block)
            })
    }
    pub fn prepare(channels: usize, impulse: &[f32]) -> Result<Arc<Self>> {
        if channels == 0 || !impulse.len().is_multiple_of(channels) {
            return Err(Error::InvalidArgument("Invalid long room FIR"));
        }
        let taps = impulse.len() / channels;
        if taps == 0 {
            return Err(Error::InvalidArgument("Empty long room FIR"));
        }
        let early_taps = taps.min(2048);
        let mut head = vec![0.; channels * early_taps];
        let tail_taps = taps.saturating_sub(early_taps);
        let mut tail = vec![0.; channels * tail_taps];
        for channel in 0..channels {
            head[channel * early_taps..(channel + 1) * early_taps]
                .copy_from_slice(&impulse[channel * taps..channel * taps + early_taps]);
            if tail_taps > 0 {
                tail[channel * tail_taps..(channel + 1) * tail_taps]
                    .copy_from_slice(&impulse[channel * taps + early_taps..(channel + 1) * taps]);
            }
        }
        Ok(Arc::new(Self {
            taps,
            channels,
            head: Filters::prepare(super::BLOCK, channels, &head)?,
            tail: if tail_taps > 0 {
                Some(Filters::prepare(2048, channels, &tail)?)
            } else {
                None
            },
        }))
    }
}

pub struct Long {
    filters: Arc<LongFilters>,
    head: Uniform,
    tail: Option<Uniform>,
    collected: Vec<f32>,
    ready: Vec<f32>,
    position: usize,
}

/// One world-field input feeds every directional FIR. Keep one FFT history
/// instead of transforming and storing that same input for every direction.
pub struct SharedLongHistory {
    head: Uniform,
    tail: Option<Uniform>,
    collected: [f32; 2048],
    position: usize,
    rendered_position: usize,
    tail_due: bool,
}
impl SharedLongHistory {
    pub fn new(filters: &LongFilters) -> Result<Self> {
        Ok(Self {
            head: Uniform::new(&filters.head)?,
            tail: filters.tail.as_ref().map(Uniform::new).transpose()?,
            collected: [0.; 2048],
            position: 0,
            rendered_position: 0,
            tail_due: false,
        })
    }
    pub fn reset(&mut self) {
        self.head.reset();
        if let Some(tail) = &mut self.tail {
            tail.reset();
        }
        self.collected.fill(0.);
        self.position = 0;
        self.rendered_position = 0;
        self.tail_due = false;
    }
    pub fn begin(&mut self, input: &[f32]) -> Result<()> {
        self.head.push(input)?;
        self.tail_due = false;
        self.rendered_position = self.position;
        if let Some(tail) = &mut self.tail {
            self.collected[self.position..self.position + super::BLOCK].copy_from_slice(input);
            self.position += super::BLOCK;
            if self.position == 2048 {
                tail.push(&self.collected)?;
                self.tail_due = true;
                self.position = 0;
            }
        }
        Ok(())
    }
}

pub struct LongReader {
    filters: Arc<LongFilters>,
    head: Uniform,
    tail: Option<Uniform>,
    ready: Vec<f32>,
}
impl LongReader {
    pub fn new(filters: Arc<LongFilters>) -> Result<Self> {
        Ok(Self {
            head: Uniform::workspace(&filters.head, false)?,
            tail: filters
                .tail
                .as_ref()
                .map(|v| Uniform::workspace(v, false))
                .transpose()?,
            ready: vec![
                0.;
                if filters.tail.is_some() {
                    filters.channels * 2048
                } else {
                    0
                }
            ],
            filters,
        })
    }
    pub fn reset(&mut self) {
        self.ready.fill(0.);
    }
    pub fn render(&mut self, history: &SharedLongHistory, output: &mut [f32]) -> Result<()> {
        self.head
            .render_shared(&self.filters.head, &history.head, output)?;
        if let (Some(tail), Some(filters), Some(input)) =
            (&mut self.tail, &self.filters.tail, &history.tail)
        {
            for (channel, out) in output.chunks_exact_mut(super::BLOCK).enumerate() {
                let begin = channel * 2048 + history.rendered_position;
                for (value, addition) in
                    out.iter_mut().zip(&self.ready[begin..begin + super::BLOCK])
                {
                    *value += *addition;
                }
            }
            if history.tail_due {
                tail.render_shared(filters, input, &mut self.ready)?;
            }
        } else if self.tail.is_some() != history.tail.is_some() {
            return Err(Error::InvalidArgument(
                "Incompatible shared room tail history",
            ));
        }
        Ok(())
    }
}

impl Long {
    pub fn new(filters: Arc<LongFilters>) -> Result<Self> {
        Ok(Self {
            head: Uniform::new(&filters.head)?,
            tail: filters.tail.as_ref().map(Uniform::new).transpose()?,
            collected: vec![0.; if filters.tail.is_some() { 2048 } else { 0 }],
            ready: vec![
                0.;
                if filters.tail.is_some() {
                    filters.channels * 2048
                } else {
                    0
                }
            ],
            filters,
            position: 0,
        })
    }
    pub fn reset(&mut self) {
        self.head.reset();
        if let Some(tail) = &mut self.tail {
            tail.reset();
        }
        self.collected.fill(0.);
        self.ready.fill(0.);
        self.position = 0;
    }
    /// Fork an identical EQ before changing its input send. The prepared filter
    /// must be the same allocation: `ready` contains already filtered samples.
    pub fn copy_history_from(&mut self, source: &Self) -> Result<()> {
        if !Arc::ptr_eq(&self.filters, &source.filters) {
            return Err(Error::InvalidArgument(
                "Cannot copy history across different room filters",
            ));
        }
        self.head.copy_history_from(&source.head)?;
        if let (Some(destination), Some(source)) = (&mut self.tail, &source.tail) {
            destination.copy_history_from(source)?;
        }
        self.collected.copy_from_slice(&source.collected);
        self.ready.copy_from_slice(&source.ready);
        self.position = source.position;
        Ok(())
    }
    pub fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        self.head.push(input)?;
        self.head.render(&self.filters.head, output)?;
        if let (Some(tail), Some(filters)) = (&mut self.tail, &self.filters.tail) {
            for (channel, out) in output.chunks_exact_mut(super::BLOCK).enumerate() {
                let begin = channel * 2048 + self.position;
                for (value, addition) in
                    out.iter_mut().zip(&self.ready[begin..begin + super::BLOCK])
                {
                    *value += *addition;
                }
            }
            self.collected[self.position..self.position + super::BLOCK].copy_from_slice(input);
            self.position += super::BLOCK;
            if self.position == 2048 {
                tail.push(&self.collected)?;
                tail.render(filters, &mut self.ready)?;
                self.position = 0;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn signal(count: usize, seed: usize) -> Vec<f32> {
        (0..count)
            .map(|i| (((i * 71 + seed) % 257) as f32 - 128.) / 512.)
            .collect()
    }
    fn reference(input: &[f32], filter: &[f32], n: usize) -> f64 {
        let mut sum = 0.;
        for (k, h) in filter.iter().enumerate() {
            if n >= k && n - k < input.len() {
                sum += f64::from(input[n - k]) * f64::from(*h);
            }
        }
        sum
    }
    #[test]
    fn target_support_reuse_matches_explicit_padding_and_rejects_invalid_input() {
        const B: usize = super::super::BLOCK;
        let taps = B * 2 + 13;
        let impulse = signal(taps * 2, 17);
        let mut dynamic = Dynamic::new(2, taps).unwrap();
        let mut reference = RealFft::new(B * 2).unwrap();
        let mut expected = vec![Complex32::default(); B + 1];
        for support in [taps, 1, B - 1, B, B + 1, taps - 1, 1] {
            dynamic.set_target_support(&impulse, support).unwrap();
            let target = &dynamic.targets[dynamic.order[dynamic.count - 1]];
            assert_eq!(target.filter.support, support);
            assert_eq!(target.filter.used_partitions, support.div_ceil(B));
            for channel in 0..2 {
                for part in 0..support.div_ceil(B) {
                    let mut explicit = vec![0.; B * 2];
                    let count = B.min(support - part * B);
                    let begin = channel * taps + part * B;
                    explicit[..count].copy_from_slice(&impulse[begin..begin + count]);
                    reference.forward(&explicit, &mut expected).unwrap();
                    let begin = (channel * target.filter.partitions + part) * (B + 1);
                    for (a, b) in target.filter.spectrum[begin..begin + B + 1]
                        .iter()
                        .zip(&expected)
                    {
                        assert_eq!(
                            (a.re.to_bits(), a.im.to_bits()),
                            (b.re.to_bits(), b.im.to_bits())
                        );
                    }
                }
            }
            let mut out = [0.; 2 * B];
            dynamic.process(&signal(B, support), &mut out).unwrap();
        }
        let order = dynamic.order;
        let count = dynamic.count;
        let index = order[count - 1];
        let spectrum = dynamic.targets[index].filter.spectrum.clone();
        for support in [0, taps + 1] {
            assert!(dynamic.set_target_support(&impulse, support).is_err());
        }
        assert!(dynamic.set_target_support(&impulse[1..], 1).is_err());
        // Nonfinite values must be rejected even outside structural support.
        for (at, value) in [(0, f32::INFINITY), (impulse.len() - 1, f32::NAN)] {
            let mut invalid = impulse.clone();
            invalid[at] = value;
            assert!(dynamic.set_target_support(&invalid, 1).is_err());
        }
        assert_eq!(dynamic.order, order);
        assert_eq!(dynamic.count, count);
        assert_eq!(dynamic.last_impulse, impulse);
        assert_eq!(dynamic.targets[index].filter.spectrum, spectrum);
        dynamic.reset();
        dynamic.set_target_support(&impulse, taps).unwrap();
        assert_eq!(dynamic.count, 1);
        assert_eq!(dynamic.targets[dynamic.order[0]].filter.support, taps);
    }

    #[test]
    fn shared_dynamic_history_preserves_stereo_stride_retarget_and_wet_fork() {
        const B: usize = super::super::BLOCK;
        let mut dry_history = InputHistory::new(1703).unwrap();
        let mut wet_history = InputHistory::new(1703).unwrap();
        let mut shared = [
            Dynamic::reader(2, 701).unwrap(),
            Dynamic::reader(1, 1703).unwrap(),
        ];
        let mut independent = [
            Dynamic::new(2, 701).unwrap(),
            Dynamic::new(1, 1703).unwrap(),
        ];
        for block in 0..60 {
            if block == 19 {
                wet_history.copy_history_from(&dry_history).unwrap();
            }
            let dry = signal(B, block * 31);
            let wet: Vec<_> = dry
                .iter()
                .map(|v| if block < 19 { *v } else { *v * 0.3 })
                .collect();
            dry_history.push(&dry, &mut shared[0].core).unwrap();
            if block >= 19 {
                wet_history.push(&wet, &mut shared[1].core).unwrap();
            }
            for (index, (reader, reference)) in shared.iter_mut().zip(&mut independent).enumerate()
            {
                let history = if index == 0 || block < 19 {
                    &dry_history
                } else {
                    &wet_history
                };
                let input = if index == 0 { &dry } else { &wet };
                let mut actual = vec![0.; B * if index == 0 { 2 } else { 1 }];
                let mut expected = actual.clone();
                reader.begin_shared();
                reference.begin(input).unwrap();
                // Retarget inside the block and cross ring wraps with unequal FIR lengths.
                for (offset, frames) in [(0, 87), (87, B - 87)] {
                    if block % 3 == 0 || block == 19 {
                        let impulse =
                            signal(if index == 0 { 1402 } else { 1703 }, block * 17 + offset);
                        reader.set_target(&impulse).unwrap();
                        reference.set_target(&impulse).unwrap();
                    }
                    reader
                        .render_shared_range(history, offset, frames, &mut actual)
                        .unwrap();
                    reference
                        .render_range(offset, frames, &mut expected)
                        .unwrap();
                }
                assert_eq!(
                    actual.iter().map(|v| v.to_bits()).collect::<Vec<_>>(),
                    expected.iter().map(|v| v.to_bits()).collect::<Vec<_>>(),
                    "block={block}, reader={index}"
                );
            }
        }
    }
    #[test]
    fn shared_input_fft_matches_independent_direction_histories() {
        for taps in [1, 2048, 2049, 10013] {
            let a = LongFilters::prepare(1, &signal(taps, 5)).unwrap();
            let b = LongFilters::prepare(1, &signal(taps, 17)).unwrap();
            let mut history = SharedLongHistory::new(&a).unwrap();
            let mut readers = [
                LongReader::new(a.clone()).unwrap(),
                LongReader::new(b.clone()).unwrap(),
            ];
            let mut independent = [Long::new(a).unwrap(), Long::new(b).unwrap()];
            let mut x = [0.; super::super::BLOCK];
            let mut y = x;
            for block in 0..70 {
                if block == 29 {
                    history.reset();
                    for (reader, reference) in readers.iter_mut().zip(&mut independent) {
                        reader.reset();
                        reference.reset();
                    }
                }
                let input = signal(super::super::BLOCK, block * 31);
                history.begin(&input).unwrap();
                for (reader, reference) in readers.iter_mut().zip(&mut independent) {
                    reader.render(&history, &mut x).unwrap();
                    reference.process(&input, &mut y).unwrap();
                    assert_eq!(
                        x.map(f32::to_bits),
                        y.map(f32::to_bits),
                        "taps={taps}, block={block}"
                    );
                }
            }
        }
    }
    #[test]
    fn nonuniform_complete_support_matches_direct_convolution() {
        let input = signal(773, 9);
        for taps in [1, 255, 256, 257, 2048, 2049, 4317] {
            let mut filter = signal(taps, 43);
            for (i, v) in filter.iter_mut().enumerate() {
                *v /= (i + 1) as f32;
            }
            let mut machine = Long::new(LongFilters::prepare(1, &filter).unwrap()).unwrap();
            let length =
                (input.len() + taps - 1).div_ceil(super::super::BLOCK) * super::super::BLOCK;
            let mut actual = vec![0.; length];
            let mut block = [0.; super::super::BLOCK];
            for start in (0..length).step_by(super::super::BLOCK) {
                block.fill(0.);
                if start < input.len() {
                    let count = block.len().min(input.len() - start);
                    block[..count].copy_from_slice(&input[start..start + count]);
                }
                machine
                    .process(&block, &mut actual[start..start + block.len()])
                    .unwrap();
            }
            for (i, sample) in actual.iter().enumerate() {
                assert!(
                    (f64::from(*sample) - reference(&input, &filter, i)).abs() < 2e-7,
                    "taps={taps}, sample={i}"
                );
            }
        }
    }
    #[test]
    fn retarget_uses_the_same_history_and_fade_survives_blocks() {
        let mut first = vec![0.; 700];
        first[5] = 1.;
        first[600] = 0.2;
        let mut second = vec![0.; 700];
        second[19] = 0.5;
        second[699] = -0.3;
        let input = signal(2048, 13);
        let mut processor = Dynamic::new(1, 700).unwrap();
        processor.set_target(&first).unwrap();
        let mut output = vec![0.; input.len()];
        for start in (0..input.len()).step_by(super::super::BLOCK) {
            if start == 512 {
                processor.set_target(&second).unwrap();
            }
            processor
                .process(
                    &input[start..start + super::super::BLOCK],
                    &mut output[start..start + super::super::BLOCK],
                )
                .unwrap();
        }
        for (i, actual) in output.iter().enumerate() {
            let alpha = ((i as f64 - 512.) / super::super::FILTER_FADE as f64).clamp(0., 1.);
            let expected =
                reference(&input, &first, i) * (1. - alpha) + reference(&input, &second, i) * alpha;
            assert!((f64::from(*actual) - expected).abs() < 3e-7, "sample={i}");
        }
    }
}
