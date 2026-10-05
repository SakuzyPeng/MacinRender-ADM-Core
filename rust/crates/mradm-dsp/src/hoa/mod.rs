//! HOA3 numerical preparation, encoding and independent metering preprocessing.
#![allow(clippy::needless_range_loop)]
pub mod coefficients;
use crate::{Error, Result, data};
use std::sync::Arc;
pub const CHANNELS: usize = 16;
pub const DECODE_CHANNELS: usize = 12;
pub const DELAY: usize = 1024;
pub const SLOTS: usize = 3;
pub const JUMP: u32 = 1;
pub const HAS_INTERPOLATION: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Source {
    pub position: [f32; 3],
    pub cartesian: u32,
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    pub gain: f32,
    pub diffuse: f32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BlockInput {
    pub start: u64,
    pub end: u64,
    pub interpolation: u64,
    pub source_offset: usize,
    pub source_count: usize,
    pub object_gain: f32,
    pub kind: u32,
    pub flags: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RowInput {
    pub input: usize,
    pub block_offset: usize,
    pub block_count: usize,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Trace {
    pub flags: u32,
    pub polar: [f32; 8],
    pub direction: [f32; 3],
    pub normalized: [f32; 3],
    pub coefficients: [f32; 16],
}
#[derive(Clone, Copy, Default)]
pub struct Coefficients {
    pub direct: [f32; 16],
    pub diffuse: [[f32; 16]; 3],
}
#[derive(Clone, Copy)]
struct Block {
    start: u64,
    end: u64,
    interpolation: Option<u64>,
    jump: bool,
    lfe: bool,
    coefficients: Coefficients,
}
struct Row {
    input: usize,
    blocks: Box<[Block]>,
    has_diffuse: bool,
    has_lfe: bool,
}
pub struct Plan {
    inputs: usize,
    rows: Vec<Row>,
    decoder: [[f32; 16]; 11],
}
pub(crate) fn invalid() -> Error {
    Error::InvalidArgument("Invalid HOA dimensions, timeline, coefficients or buffers")
}
fn product(a: usize, b: usize) -> Result<usize> {
    a.checked_mul(b)
        .filter(|n| *n <= isize::MAX as usize / size_of::<f32>())
        .ok_or_else(invalid)
}
fn subrange<T>(values: &[T], offset: usize, count: usize) -> Result<&[T]> {
    if offset > values.len() || count > values.len() - offset {
        return Err(invalid());
    }
    Ok(&values[offset..offset + count])
}
impl Plan {
    pub fn new(
        inputs: usize,
        rows: &[RowInput],
        blocks: &[BlockInput],
        order: &[usize],
        sources: &[Source],
    ) -> Result<(Self, Trace)> {
        if inputs == 0 || order.len() != blocks.len() {
            return Err(invalid());
        }
        product(blocks.len(), 64)?;
        let mut trace = Trace::default();
        let mut compiled = Vec::with_capacity(blocks.len());
        // Compile in original scene traversal order, before applying C++'s timeline permutation.
        for b in blocks {
            if b.flags & !3 != 0 {
                return Err(invalid());
            }
            let source = subrange(sources, b.source_offset, b.source_count)?;
            let coefficients = coefficients::compile(b.kind, source, b.object_gain, &mut trace)?;
            compiled.push(Block {
                start: b.start,
                end: b.end.max(b.start),
                interpolation: if b.flags & HAS_INTERPOLATION != 0 {
                    Some(b.interpolation)
                } else {
                    None
                },
                jump: b.flags & JUMP != 0,
                lfe: b.kind == 2,
                coefficients,
            });
        }
        let mut seen = vec![false; blocks.len()];
        let mut prepared = Vec::with_capacity(rows.len());
        let mut expected_offset = 0;
        for row in rows {
            if row.input >= inputs || row.block_offset != expected_offset {
                return Err(invalid());
            }
            let ids = subrange(order, row.block_offset, row.block_count)?;
            expected_offset += row.block_count;
            let mut table = Vec::with_capacity(ids.len());
            for &id in ids {
                if id >= compiled.len() || seen[id] {
                    return Err(invalid());
                }
                seen[id] = true;
                let block = compiled[id];
                if table.last().is_some_and(|b: &Block| b.start > block.start) {
                    return Err(invalid());
                }
                table.push(block);
            }
            let has_diffuse = table
                .iter()
                .any(|b| b.coefficients.diffuse.iter().any(|s| s[0].abs() > 0.));
            let has_lfe = table.iter().any(|b| b.lfe);
            prepared.push(Row {
                input: row.input,
                blocks: table.into_boxed_slice(),
                has_diffuse,
                has_lfe,
            });
        }
        if expected_offset != order.len() {
            return Err(invalid());
        }
        let mut decoder = [[0.; 16]; 11];
        for (v, bytes) in decoder
            .iter_mut()
            .flatten()
            .zip(data::HOA_714.as_chunks::<4>().0)
        {
            *v = f32::from_le_bytes(*bytes);
        }
        Ok((
            Self {
                inputs,
                rows: prepared,
                decoder,
            },
            trace,
        ))
    }
    pub fn inputs(&self) -> usize {
        self.inputs
    }
    pub fn rows(&self) -> usize {
        self.rows.len()
    }
    pub fn has_lfe(&self) -> bool {
        self.rows.iter().any(|r| r.has_lfe)
    }
    pub fn coefficients(&self, row: usize, block: usize) -> Result<Coefficients> {
        self.rows
            .get(row)
            .and_then(|r| r.blocks.get(block))
            .map(|b| b.coefficients)
            .ok_or_else(invalid)
    }
}
#[derive(Clone, Copy, Default)]
struct Cursor {
    next: usize,
    last: Option<u64>,
}
fn gains(
    row: &Row,
    cursor: &mut Cursor,
    frame: u64,
    default_interpolation: u64,
    lfe_only: bool,
) -> Coefficients {
    let blocks = &row.blocks;
    if cursor.last.is_none_or(|last| frame < last) {
        cursor.next = blocks.partition_point(|b| b.start <= frame);
    } else {
        while cursor.next < blocks.len() && blocks[cursor.next].start <= frame {
            cursor.next += 1;
        }
    }
    cursor.last = Some(frame);
    if cursor.next == 0 {
        return Coefficients::default();
    }
    let index = cursor.next - 1;
    let current = blocks[index];
    if frame >= current.end || (lfe_only && !current.lfe) {
        return Coefficients::default();
    }
    if !current.jump && index > 0 {
        let previous = blocks[index - 1];
        if lfe_only && !previous.lfe {
            return current.coefficients;
        }
        let active_end = blocks
            .get(index + 1)
            .map_or(current.end, |b| current.end.min(b.start));
        let length = current
            .interpolation
            .unwrap_or(default_interpolation)
            .min(active_end.saturating_sub(current.start));
        let delta = frame - current.start;
        if length > 0 && delta < length {
            let alpha = delta as f64 / length as f64;
            let mut out = Coefficients::default();
            for i in 0..16 {
                out.direct[i] = (f64::from(previous.coefficients.direct[i]) * (1. - alpha)
                    + f64::from(current.coefficients.direct[i]) * alpha)
                    as f32;
            }
            for slot in 0..3 {
                for i in 0..16 {
                    out.diffuse[slot][i] = (f64::from(previous.coefficients.diffuse[slot][i])
                        * (1. - alpha)
                        + f64::from(current.coefficients.diffuse[slot][i]) * alpha)
                        as f32;
                }
            }
            return out;
        }
    }
    current.coefficients
}
pub struct Diffuse {
    history: Vec<[f32; 16]>,
    write: usize,
    weight: f32,
}
impl Default for Diffuse {
    fn default() -> Self {
        Self {
            history: vec![[0.; 16]; 1024],
            write: 0,
            weight: 1. / 32f32.sqrt(),
        }
    }
}
impl Diffuse {
    pub fn reset(&mut self, start: u64) {
        self.history.fill([0.; 16]);
        self.write = (start % 1024) as usize;
    }
    pub fn add(&mut self, input: [f32; 16], output: &mut [f32; 16]) {
        const DELAYS: [usize; 32] = [
            37, 53, 67, 83, 97, 109, 127, 149, 163, 181, 199, 211, 233, 251, 271, 293, 313, 337,
            359, 383, 409, 431, 457, 487, 521, 557, 593, 631, 673, 719, 761, 809,
        ];
        const POLARITY: [f32; 32] = [
            1., -1., 1., 1., -1., -1., 1., -1., -1., 1., 1., -1., 1., -1., -1., 1., -1., 1., 1.,
            -1., 1., -1., -1., 1., 1., -1., 1., -1., -1., 1., -1., 1.,
        ];
        let mut acc = [0.; 16];
        for tap in 0..32 {
            let read = (self.write + 1024 - DELAYS[tap]) % 1024;
            for sh in 0..16 {
                acc[sh] += self.history[read][sh] * POLARITY[tap] * self.weight;
            }
        }
        for sh in 0..16 {
            output[sh] += acc[sh];
        }
        self.history[self.write] = input;
        self.write = (self.write + 1) % 1024;
    }
}
pub struct Encoder {
    plan: Arc<Plan>,
    cursors: Vec<Cursor>,
    diffuse: Vec<Option<[Diffuse; 3]>>,
    endpoints: [Coefficients; 2],
    max_frames: usize,
    interpolation: u64,
    smoothing: bool,
}
impl Encoder {
    pub fn new(
        plan: Arc<Plan>,
        max_frames: usize,
        interpolation: u64,
        smoothing: bool,
    ) -> Result<Self> {
        if max_frames == 0 {
            return Err(invalid());
        }
        product(max_frames, plan.inputs)?;
        product(max_frames, 16)?;
        let diffuse = plan
            .rows
            .iter()
            .map(|r| {
                if r.has_diffuse {
                    Some(std::array::from_fn(|_| Diffuse::default()))
                } else {
                    None
                }
            })
            .collect();
        Ok(Self {
            cursors: vec![Cursor::default(); plan.rows.len()],
            diffuse,
            endpoints: [Coefficients::default(); 2],
            max_frames,
            interpolation,
            smoothing,
            plan,
        })
    }
    pub fn reset(&mut self, start: u64) {
        self.cursors.fill(Cursor::default());
        self.endpoints.fill(Coefficients::default());
        for slots in self.diffuse.iter_mut().flatten() {
            for slot in slots {
                slot.reset(start);
            }
        }
    }
    pub fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        start: u64,
        frames: usize,
    ) -> Result<()> {
        validate(
            &self.plan,
            self.max_frames,
            input,
            output.len(),
            16,
            start,
            frames,
        )?;
        if frames == 0 {
            return Ok(());
        }
        for (ci, row) in self.plan.rows.iter().enumerate() {
            if self.smoothing {
                self.endpoints[0] =
                    gains(row, &mut self.cursors[ci], start, self.interpolation, false);
                self.endpoints[1] = gains(
                    row,
                    &mut self.cursors[ci],
                    start + frames as u64 - 1,
                    self.interpolation,
                    false,
                );
            }
            for f in 0..frames {
                let g = if self.smoothing {
                    let alpha = if frames > 1 {
                        f as f32 / (frames - 1) as f32
                    } else {
                        0.
                    };
                    let mut g = Coefficients::default();
                    for i in 0..16 {
                        g.direct[i] = self.endpoints[0].direct[i] * (1. - alpha)
                            + self.endpoints[1].direct[i] * alpha;
                    }
                    for slot in 0..3 {
                        for i in 0..16 {
                            g.diffuse[slot][i] = self.endpoints[0].diffuse[slot][i] * (1. - alpha)
                                + self.endpoints[1].diffuse[slot][i] * alpha;
                        }
                    }
                    g
                } else {
                    gains(
                        row,
                        &mut self.cursors[ci],
                        start + f as u64,
                        self.interpolation,
                        false,
                    )
                };
                let sample = input[f * self.plan.inputs + row.input];
                let out: &mut [f32; 16] = (&mut output[f * 16..(f + 1) * 16]).try_into().unwrap();
                for i in 0..16 {
                    out[i] += sample * g.direct[i];
                }
                if let Some(slots) = &mut self.diffuse[ci] {
                    for (slot, state) in slots.iter_mut().enumerate() {
                        let mut scaled = g.diffuse[slot];
                        for value in &mut scaled {
                            *value *= sample;
                        }
                        state.add(scaled, out);
                    }
                }
            }
        }
        Ok(())
    }
}
fn validate(
    plan: &Plan,
    max: usize,
    input: &[f32],
    out_len: usize,
    channels: usize,
    start: u64,
    frames: usize,
) -> Result<()> {
    if frames > max
        || start.checked_add(frames as u64).is_none()
        || !input.len().is_multiple_of(plan.inputs)
        || input.len() < product(frames, plan.inputs)?
        || !out_len.is_multiple_of(channels)
        || out_len < product(frames, channels)?
    {
        return Err(invalid());
    }
    Ok(())
}
pub struct MeterPreprocessor {
    plan: Arc<Plan>,
    cursors: Vec<Cursor>,
    scratch: Vec<f32>,
    max_frames: usize,
    interpolation: u64,
}
impl MeterPreprocessor {
    pub fn new(plan: Arc<Plan>, max_frames: usize, interpolation: u64) -> Result<Self> {
        if max_frames == 0 {
            return Err(invalid());
        }
        product(max_frames, plan.inputs)?;
        Ok(Self {
            cursors: vec![Cursor::default(); plan.rows.len()],
            scratch: vec![0.; product(max_frames, 16)?],
            max_frames,
            interpolation,
            plan,
        })
    }
    pub fn reset(&mut self, _start: u64) {
        self.cursors.fill(Cursor::default());
        self.scratch.fill(0.);
    }
    #[allow(clippy::too_many_arguments)]
    pub fn process(
        &mut self,
        input: &[f32],
        encoded: &[f32],
        decoded: &mut [f32],
        lfe: &mut [f32],
        start: u64,
        frames: usize,
    ) -> Result<()> {
        validate(
            &self.plan,
            self.max_frames,
            input,
            decoded.len(),
            12,
            start,
            frames,
        )?;
        if !encoded.len().is_multiple_of(16)
            || encoded.len() < product(frames, 16)?
            || lfe.len() < frames
        {
            return Err(invalid());
        }
        if frames == 0 {
            return Ok(());
        }
        self.scratch[..frames * 16].copy_from_slice(&encoded[..frames * 16]);
        lfe[..frames].fill(0.);
        for (ci, row) in self.plan.rows.iter().enumerate() {
            if !row.has_lfe {
                continue;
            }
            for f in 0..frames {
                let sample = input[f * self.plan.inputs + row.input];
                let coeff = gains(
                    row,
                    &mut self.cursors[ci],
                    start + f as u64,
                    self.interpolation,
                    true,
                );
                lfe[f] += sample * coeff.direct[0];
                for sh in 0..16 {
                    self.scratch[f * 16 + sh] -= sample * coeff.direct[sh];
                }
            }
        }
        for f in 0..frames {
            for ls in 0..11 {
                let mut sum = 0.;
                for sh in 0..16 {
                    sum += self.plan.decoder[ls][sh] * self.scratch[f * 16 + sh];
                }
                decoded[f * 12 + if ls < 3 { ls } else { ls + 1 }] = sum;
            }
            decoded[f * 12 + 3] = 0.;
        }
        Ok(())
    }
}
