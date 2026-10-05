//! Prepared PCM mixing. ADM identities, coefficient design and I/O stay outside this module.
use crate::{Error, Result};
use std::sync::Arc;

pub const JUMP: u32 = 1;
pub const SMOOTH: u32 = 2;
pub const HAS_INTERPOLATION: u32 = 4;

#[derive(Clone, Copy, Default)]
pub struct Block {
    pub start: u64,
    pub end: u64,
    pub interpolation: u64,
    pub flags: u32,
}
#[derive(Clone, Copy)]
pub struct Row {
    pub input_channel: usize,
    pub block_offset: usize,
    pub block_count: usize,
    pub output_gain: f32,
}
pub enum Coefficients {
    Speaker(Vec<f32>),
    /// Each block contains direct[outputs], followed by diffuse[outputs].
    Ear(Vec<f64>),
}
pub struct Plan {
    inputs: usize,
    outputs: usize,
    rows: Vec<Row>,
    blocks: Vec<Block>,
    coefficients: Coefficients,
}
fn invalid() -> Error {
    Error::InvalidArgument("Invalid PCM mixing dimensions, timeline or coefficients")
}
fn product(a: usize, b: usize) -> Result<usize> {
    a.checked_mul(b)
        .filter(|&n| n <= isize::MAX as usize / size_of::<f64>())
        .ok_or_else(invalid)
}
fn valid_block(block: Block) -> bool {
    block.end >= block.start && block.flags & !7 == 0
}

impl Plan {
    pub fn new(
        inputs: usize,
        outputs: usize,
        rows: Vec<Row>,
        blocks: Vec<Block>,
        coefficients: Coefficients,
    ) -> Result<Self> {
        if inputs == 0 || outputs == 0 {
            return Err(invalid());
        }
        let count = product(blocks.len(), outputs)?;
        let valid = match &coefficients {
            Coefficients::Speaker(v) => v.len() == count && v.iter().all(|v| v.is_finite()),
            Coefficients::Ear(v) => {
                v.len() == product(count, 2)?
                    && v.iter().all(|v| v.is_finite() && (*v as f32).is_finite())
            }
        };
        if !valid || blocks.iter().any(|b| !valid_block(*b)) {
            return Err(invalid());
        }
        for row in &rows {
            if row.input_channel >= inputs
                || !row.output_gain.is_finite()
                || row.block_offset > blocks.len()
                || row.block_count > blocks.len() - row.block_offset
            {
                return Err(invalid());
            }
            let slice = &blocks[row.block_offset..row.block_offset + row.block_count];
            if slice.iter().any(|b| !valid_block(*b))
                || slice.windows(2).any(|b| b[0].start > b[1].start)
            {
                return Err(invalid());
            }
        }
        Ok(Self {
            inputs,
            outputs,
            rows,
            blocks,
            coefficients,
        })
    }
    pub fn inputs(&self) -> usize {
        self.inputs
    }
    pub fn outputs(&self) -> usize {
        self.outputs
    }
    pub fn rows(&self) -> usize {
        self.rows.len()
    }
    pub fn is_ear(&self) -> bool {
        matches!(self.coefficients, Coefficients::Ear(_))
    }
}

enum Table {
    Shared(Arc<Plan>),
    Dynamic(Plan),
}
impl Table {
    fn plan(&self) -> &Plan {
        match self {
            Self::Shared(p) => p,
            Self::Dynamic(p) => p,
        }
    }
}
struct Scratch {
    start: Vec<f32>,
    end: Vec<f32>,
    start_diffuse: Vec<f32>,
    end_diffuse: Vec<f32>,
    mono: Vec<f32>,
    direct: Vec<f32>,
    diffuse: Vec<f32>,
}
pub struct Mixer {
    table: Table,
    indices: Vec<usize>,
    max_frames: usize,
    default_interpolation: u64,
    smoothing: bool,
    dynamic_capacity: usize,
    scratch: Scratch,
}

impl Mixer {
    pub fn new(
        plan: Arc<Plan>,
        max_frames: usize,
        default_interpolation: u64,
        smoothing: bool,
    ) -> Result<Self> {
        Self::prepare(
            Table::Shared(plan),
            max_frames,
            default_interpolation,
            smoothing,
            0,
        )
    }
    pub fn dynamic(
        inputs: usize,
        outputs: usize,
        input_channels: &[usize],
        capacity: usize,
        max_frames: usize,
        default_interpolation: u64,
    ) -> Result<Self> {
        if capacity == 0 {
            return Err(invalid());
        }
        let blocks = product(input_channels.len(), capacity)?;
        let gains = product(blocks, outputs)?;
        let rows = input_channels
            .iter()
            .enumerate()
            .map(|(i, &input_channel)| Row {
                input_channel,
                block_offset: i * capacity,
                block_count: 0,
                output_gain: 1.,
            })
            .collect();
        let plan = Plan::new(
            inputs,
            outputs,
            rows,
            vec![Block::default(); blocks],
            Coefficients::Speaker(vec![0.; gains]),
        )?;
        Self::prepare(
            Table::Dynamic(plan),
            max_frames,
            default_interpolation,
            false,
            capacity,
        )
    }
    fn prepare(
        table: Table,
        max_frames: usize,
        default_interpolation: u64,
        smoothing: bool,
        dynamic_capacity: usize,
    ) -> Result<Self> {
        if max_frames == 0 {
            return Err(invalid());
        }
        let plan = table.plan();
        product(max_frames, plan.inputs)?;
        let samples = product(max_frames, plan.outputs)?;
        let ear = plan.is_ear();
        let scratch = Scratch {
            start: vec![0.; plan.outputs],
            end: vec![0.; plan.outputs],
            start_diffuse: vec![0.; if ear { plan.outputs } else { 0 }],
            end_diffuse: vec![0.; if ear { plan.outputs } else { 0 }],
            mono: vec![0.; if ear { max_frames } else { 0 }],
            direct: vec![0.; if ear { samples } else { 0 }],
            diffuse: vec![0.; if ear { samples } else { 0 }],
        };
        let indices = vec![0; plan.rows.len()];
        Ok(Self {
            table,
            indices,
            max_frames,
            default_interpolation,
            smoothing,
            dynamic_capacity,
            scratch,
        })
    }
    pub fn inputs(&self) -> usize {
        self.table.plan().inputs
    }
    pub fn outputs(&self) -> usize {
        self.table.plan().outputs
    }
    pub fn rows(&self) -> usize {
        self.indices.len()
    }
    pub fn is_ear(&self) -> bool {
        self.table.plan().is_ear()
    }
    pub fn reset(&mut self) {
        self.indices.fill(0);
        self.scratch.start.fill(0.);
        self.scratch.end.fill(0.);
        self.scratch.start_diffuse.fill(0.);
        self.scratch.end_diffuse.fill(0.);
        self.scratch.mono.fill(0.);
        self.scratch.direct.fill(0.);
        self.scratch.diffuse.fill(0.);
    }
    /// The descriptor iterator is validated once, then copied into fixed storage without allocation.
    pub fn update<I: ExactSizeIterator<Item = Block> + Clone>(
        &mut self,
        row: usize,
        blocks: I,
        gains: &[f32],
        output_gain: f32,
    ) -> Result<()> {
        let Table::Dynamic(plan) = &mut self.table else {
            return Err(invalid());
        };
        if row >= plan.rows.len()
            || blocks.len() > self.dynamic_capacity
            || !output_gain.is_finite()
            || gains.len() != product(blocks.len(), plan.outputs)?
            || gains.iter().any(|v| !v.is_finite())
        {
            return Err(invalid());
        }
        let mut previous = None;
        for block in blocks.clone() {
            if !valid_block(block) || previous.is_some_and(|v| v > block.start) {
                return Err(invalid());
            }
            previous = Some(block.start);
        }
        let target = &mut plan.rows[row];
        target.block_count = blocks.len();
        target.output_gain = output_gain;
        for (index, block) in blocks.enumerate() {
            plan.blocks[target.block_offset + index] = block;
        }
        let Coefficients::Speaker(coefficients) = &mut plan.coefficients else {
            unreachable!()
        };
        let start = target.block_offset * plan.outputs;
        coefficients[start..start + gains.len()].copy_from_slice(gains);
        self.indices[row] = 0;
        Ok(())
    }
    fn validate(&self, input: &[f32], output: &[f32], start: u64, frames: usize) -> Result<()> {
        if frames > self.max_frames
            || start.checked_add(frames as u64).is_none()
            || !input.len().is_multiple_of(self.inputs())
            || !output.len().is_multiple_of(self.outputs())
            || input.len() < product(frames, self.inputs())?
            || output.len() < product(frames, self.outputs())?
        {
            return Err(invalid());
        }
        Ok(())
    }
    /// Add all rows, or one row without extra object smoothing. A gain override serves the dry point branch.
    #[allow(clippy::too_many_arguments)]
    pub fn speaker(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        live: &[f32],
        start: u64,
        frames: usize,
        row: Option<usize>,
        output_gain: Option<f32>,
    ) -> Result<()> {
        self.validate(input, output, start, frames)?;
        if self.is_ear()
            || row.is_some_and(|r| r >= self.rows())
            || output_gain.is_some_and(|v| !v.is_finite())
            || (!live.is_empty()
                && (!live.len().is_multiple_of(self.inputs())
                    || live.len() < frames * self.inputs()))
        {
            return Err(invalid());
        }
        if frames == 0 {
            return Ok(());
        }
        let plan = self.table.plan();
        let Coefficients::Speaker(gains) = &plan.coefficients else {
            unreachable!()
        };
        // Dynamic point curves used a fresh local cursor for every accumulation in C++.
        // In particular, the dry branch may reuse the same absolute window with another gain.
        let dynamic = matches!(self.table, Table::Dynamic(_));
        let range = row.map_or(0..plan.rows.len(), |r| r..r + 1);
        for r in range {
            let channel = plan.rows[r];
            if channel.block_count == 0 {
                continue;
            }
            let blocks =
                &plan.blocks[channel.block_offset..channel.block_offset + channel.block_count];
            let coefficients = &gains[channel.block_offset * plan.outputs..];
            let index = &mut self.indices[r];
            if dynamic {
                *index = 0;
            }
            let mut first = *index;
            let mut last = *index;
            let smooth = self.smoothing
                && row.is_none()
                && speaker_at(
                    blocks,
                    coefficients,
                    plan.outputs,
                    &mut first,
                    start,
                    self.default_interpolation,
                    &mut self.scratch.start,
                )
                && speaker_at(
                    blocks,
                    coefficients,
                    plan.outputs,
                    &mut last,
                    start + frames as u64 - 1,
                    self.default_interpolation,
                    &mut self.scratch.end,
                );
            if smooth {
                *index = first;
            }
            let scalar = output_gain.unwrap_or(channel.output_gain);
            for f in 0..frames {
                let absolute = start + f as u64;
                if !smooth {
                    advance(blocks, index, absolute);
                    if !active(blocks[*index], absolute) {
                        continue;
                    }
                }
                let sample = input[f * plan.inputs + channel.input_channel];
                let length = interpolation(blocks, *index, self.default_interpolation);
                let delta = absolute - blocks[*index].start;
                for c in 0..plan.outputs {
                    let gain = if smooth {
                        let alpha = if frames > 1 {
                            f as f32 / (frames - 1) as f32
                        } else {
                            0.
                        };
                        self.scratch.start[c] * (1. - alpha) + self.scratch.end[c] * alpha
                    } else {
                        let current = coefficients[*index * plan.outputs + c];
                        if length > 0 && delta < length {
                            interpolate(
                                coefficients[(*index - 1) * plan.outputs + c] as f64,
                                current as f64,
                                delta,
                                length,
                            ) as f32
                        } else {
                            current
                        }
                    };
                    let mut contribution = (sample * gain) * scalar;
                    if !live.is_empty() {
                        contribution *= live[f * plan.inputs + channel.input_channel];
                    }
                    output[f * plan.outputs + c] += contribution;
                }
            }
        }
        Ok(())
    }
    pub fn ear(
        &mut self,
        input: &[f32],
        direct: &mut [f32],
        diffuse: &mut [f32],
        start: u64,
        frames: usize,
    ) -> Result<()> {
        self.validate(input, direct, start, frames)?;
        self.validate(input, diffuse, start, frames)?;
        let plan = self.table.plan();
        let Coefficients::Ear(gains) = &plan.coefficients else {
            return Err(invalid());
        };
        if frames == 0 {
            return Ok(());
        }
        let scratch = &mut self.scratch;
        scratch.direct.fill(0.);
        scratch.diffuse.fill(0.);
        for (r, channel) in plan.rows.iter().enumerate() {
            if channel.block_count == 0 {
                continue;
            }
            let blocks =
                &plan.blocks[channel.block_offset..channel.block_offset + channel.block_count];
            let coefficients = &gains[channel.block_offset * plan.outputs * 2..];
            for f in 0..frames {
                scratch.mono[f] = input[f * plan.inputs + channel.input_channel];
            }
            let index = &mut self.indices[r];
            let mut first = *index;
            let mut last = *index;
            let smooth = self.smoothing
                && ear_at(
                    blocks,
                    coefficients,
                    plan.outputs,
                    &mut first,
                    start,
                    self.default_interpolation,
                    &mut scratch.start,
                    &mut scratch.start_diffuse,
                )
                && ear_at(
                    blocks,
                    coefficients,
                    plan.outputs,
                    &mut last,
                    start + frames as u64 - 1,
                    self.default_interpolation,
                    &mut scratch.end,
                    &mut scratch.end_diffuse,
                );
            if smooth {
                *index = first;
                for c in 0..plan.outputs {
                    for f in 0..frames {
                        let alpha = if frames > 1 {
                            f as f32 / (frames - 1) as f32
                        } else {
                            0.
                        };
                        let gd = scratch.start[c] * (1. - alpha) + scratch.end[c] * alpha;
                        let gf = scratch.start_diffuse[c] * (1. - alpha)
                            + scratch.end_diffuse[c] * alpha;
                        scratch.direct[c * self.max_frames + f] += scratch.mono[f] * gd;
                        scratch.diffuse[c * self.max_frames + f] += scratch.mono[f] * gf;
                    }
                }
                continue;
            }
            let mut f0 = 0;
            while f0 < frames {
                let absolute = start + f0 as u64;
                advance(blocks, index, absolute);
                let block = blocks[*index];
                if absolute < block.start {
                    f0 = (block.start - start).min(frames as u64) as usize;
                    continue;
                }
                if absolute >= block.end {
                    if *index + 1 >= blocks.len() {
                        break;
                    }
                    f0 = (absolute.max(blocks[*index + 1].start) - start).min(frames as u64)
                        as usize;
                    *index += 1;
                    continue;
                }
                let mut end = block.end.min(start + frames as u64);
                if *index + 1 < blocks.len() {
                    end = end.min(blocks[*index + 1].start);
                }
                let f1 = (end - start) as usize;
                let length = interpolation(blocks, *index, self.default_interpolation);
                let any_ramp = length > 0 && absolute - block.start < length;
                if !any_ramp {
                    for c in 0..plan.outputs {
                        let gd = coefficients[*index * plan.outputs * 2 + c] as f32;
                        let gf = coefficients[*index * plan.outputs * 2 + plan.outputs + c] as f32;
                        if gd != 0. {
                            for f in f0..f1 {
                                scratch.direct[c * self.max_frames + f] += scratch.mono[f] * gd;
                            }
                        }
                        if gf != 0. {
                            for f in f0..f1 {
                                scratch.diffuse[c * self.max_frames + f] += scratch.mono[f] * gf;
                            }
                        }
                    }
                } else {
                    for f in f0..f1 {
                        let delta = start + f as u64 - block.start;
                        for c in 0..plan.outputs {
                            let mut gd = coefficients[*index * plan.outputs * 2 + c];
                            let mut gf = coefficients[*index * plan.outputs * 2 + plan.outputs + c];
                            if delta < length {
                                gd = interpolate(
                                    coefficients[(*index - 1) * plan.outputs * 2 + c],
                                    gd,
                                    delta,
                                    length,
                                );
                                gf = interpolate(
                                    coefficients
                                        [(*index - 1) * plan.outputs * 2 + plan.outputs + c],
                                    gf,
                                    delta,
                                    length,
                                );
                            }
                            scratch.direct[c * self.max_frames + f] += scratch.mono[f] * gd as f32;
                            scratch.diffuse[c * self.max_frames + f] += scratch.mono[f] * gf as f32;
                        }
                    }
                }
                f0 = f1;
            }
        }
        for c in 0..plan.outputs {
            for f in 0..frames {
                direct[f * plan.outputs + c] = scratch.direct[c * self.max_frames + f];
                diffuse[f * plan.outputs + c] = scratch.diffuse[c * self.max_frames + f];
            }
        }
        Ok(())
    }
}

fn advance(blocks: &[Block], index: &mut usize, absolute: u64) {
    while *index + 1 < blocks.len() && absolute >= blocks[*index + 1].start {
        *index += 1;
    }
}
fn active(block: Block, absolute: u64) -> bool {
    absolute >= block.start && absolute < block.end
}
fn interpolation(blocks: &[Block], index: usize, default: u64) -> u64 {
    let block = blocks[index];
    if index == 0 || block.flags & JUMP != 0 {
        return 0;
    }
    let end = if index + 1 < blocks.len() {
        block.end.min(blocks[index + 1].start)
    } else {
        block.end
    };
    let wanted = if block.flags & HAS_INTERPOLATION != 0 {
        block.interpolation
    } else {
        default
    };
    wanted.min(end.saturating_sub(block.start))
}
fn interpolate(previous: f64, current: f64, delta: u64, length: u64) -> f64 {
    let alpha = delta as f64 / length as f64;
    previous * (1. - alpha) + current * alpha
}
fn speaker_at(
    blocks: &[Block],
    gains: &[f32],
    outputs: usize,
    index: &mut usize,
    absolute: u64,
    default: u64,
    result: &mut [f32],
) -> bool {
    result.fill(0.);
    advance(blocks, index, absolute);
    if !active(blocks[*index], absolute) {
        return false;
    }
    let length = interpolation(blocks, *index, default);
    let delta = absolute - blocks[*index].start;
    for c in 0..outputs {
        let current = gains[*index * outputs + c];
        result[c] = if length > 0 && delta < length {
            interpolate(
                gains[(*index - 1) * outputs + c] as f64,
                current as f64,
                delta,
                length,
            ) as f32
        } else {
            current
        };
    }
    blocks[*index].flags & SMOOTH != 0
}
#[allow(clippy::too_many_arguments)]
fn ear_at(
    blocks: &[Block],
    gains: &[f64],
    outputs: usize,
    index: &mut usize,
    absolute: u64,
    default: u64,
    direct: &mut [f32],
    diffuse: &mut [f32],
) -> bool {
    direct.fill(0.);
    diffuse.fill(0.);
    advance(blocks, index, absolute);
    if !active(blocks[*index], absolute) {
        return false;
    }
    let length = interpolation(blocks, *index, default);
    let delta = absolute - blocks[*index].start;
    for (bus, result) in [direct, diffuse].into_iter().enumerate() {
        for c in 0..outputs {
            let current = gains[*index * outputs * 2 + bus * outputs + c] as f32;
            result[c] = if length > 0 && delta < length {
                let previous = gains[(*index - 1) * outputs * 2 + bus * outputs + c] as f32;
                interpolate(previous as f64, current as f64, delta, length) as f32
            } else {
                current
            };
        }
    }
    blocks[*index].flags & SMOOTH != 0
}

pub struct Matrix {
    inputs: usize,
    outputs: usize,
    gains: Vec<f32>,
}
impl Matrix {
    pub fn new(inputs: usize, outputs: usize, gains: &[f32]) -> Result<Self> {
        if inputs == 0
            || outputs == 0
            || gains.len() != product(inputs, outputs)?
            || gains.iter().any(|v| !v.is_finite())
        {
            return Err(invalid());
        }
        Ok(Self {
            inputs,
            outputs,
            gains: gains.to_vec(),
        })
    }
    pub fn process(&self, input: &[f32], output: &mut [f32], frames: usize) -> Result<()> {
        if !input.len().is_multiple_of(self.inputs)
            || !output.len().is_multiple_of(self.outputs)
            || input.len() < product(frames, self.inputs)?
            || output.len() < product(frames, self.outputs)?
        {
            return Err(invalid());
        }
        for f in 0..frames {
            for d in 0..self.outputs {
                let mut sum = 0.;
                for s in 0..self.inputs {
                    sum += self.gains[d * self.inputs + s] * input[f * self.inputs + s];
                }
                output[f * self.outputs + d] = sum;
            }
        }
        Ok(())
    }
}
