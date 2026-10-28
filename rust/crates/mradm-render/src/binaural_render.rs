//! Binaural rendering (PCM consistency 4.7 B), moved from the C++ `binaural_renderer.cpp` and
//! `binaural_spreader.cpp`: the per-source OLA convolution (`render_source_ola_block`, with its
//! HRTF cache, head-rotation crossfade, object smoothing and LFE bypass), the SAF spreader adapter
//! (512-frame batches, output ring, one-frame prime) and the offline render loop (OLA reduction in
//! source order, the spreader latency compensation, the output window and the meter). Sources and
//! spreader groups render on worker threads into their own buffers and are reduced in index
//! order, so the worker count never changes the output.
use crate::binaural::{self, Group, Input, Log, Source, Spread, Track};
use crate::binaural_hrtf::State;
use crate::offline::{Error, Location, Metrics, Progress, Result, Window};
use crate::routing::{self, Level};
use mradm_dsp::convolution::OlaConvolver;
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::diffuse::{DELAY_LENGTH, DiffuseDelay};
use mradm_dsp::filterbank::{DELAY, FRAME};
use mradm_dsp::hrtf_filters::{Filters, Lookup, Trace, grid_index};
use mradm_dsp::meter::{Meter, MeterMode};
use mradm_dsp::scene_math::Rotation;
use mradm_dsp::spreader::Spreader;
use mradm_wav::Reader;
use rayon::prelude::*;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;

pub const MIN_BLOCK: u64 = 1024;

fn dsp(e: &mradm_dsp::Error) -> Error {
    Error::dsp(e, Location::None)
}

/// A preparation: the HRTF state, the plan input and the sources and spreader groups.
pub struct Prepared {
    pub hrtf: Arc<State>,
    pub input: Arc<Input>,
    pub spread: Spread,
    pub sources: Vec<Source>,
    pub tracks: Vec<Track>,
    pub groups: Vec<Group>,
    pub block: u64,
}

/// Plans the sources (and spreader groups) with the C++ log lines after the HRTF source line.
pub fn prepare(
    scene: &mradm_scene::RenderScene,
    hrtf: Arc<State>,
    spread: Spread,
    smoothing_frames: u32,
    log: &mut Log,
) -> std::result::Result<Prepared, routing::Error> {
    let input = Arc::new(Input::of(scene));
    let sources = binaural::sources(&input, spread, &BTreeMap::new(), log);
    if sources.is_empty() {
        log(
            Level::Warning,
            b"no renderable tracks found, writing silence".to_vec(),
        );
    }
    log(
        Level::Info,
        format!("{} source(s) to render", sources.len()).into_bytes(),
    );
    let (mut tracks, mut groups) = (Vec::new(), Vec::new());
    if spread == Spread::SafSpreader {
        let budget = binaural::group_budget().map_err(|message| routing::Error {
            code: 4,
            message: message.into_bytes(),
            context: Vec::new(),
        })?;
        tracks = binaural::tracks(&input, log);
        groups = binaural::groups(&tracks, budget);
        log(
            Level::Info,
            format!(
                "{} spreader track(s) in {} adapter group(s) (saf_spreader, compensated latency {} \
                 samples)",
                tracks.len(),
                groups.len(),
                total_latency()
            )
            .into_bytes(),
        );
    }
    Ok(Prepared {
        hrtf,
        input,
        spread,
        sources,
        tracks,
        groups,
        block: u64::from(smoothing_frames).max(MIN_BLOCK),
    })
}

// ── SAF spreader adapter ──────────────────────────────────────────────────────

const RING: usize = 32768;

/// The spreader's constant latency after `prime`: its STFT delay plus the one-frame cushion.
pub fn total_latency() -> usize {
    DELAY + FRAME
}

/// One OM spreader with up to eight source lanes over variable render chunks. Output accumulates
/// into the caller's buffers.
pub struct Adapter {
    spreader: Spreader,
    lanes: usize,
    gains: [f32; binaural::MAX_LANES],
    carry: Vec<f32>,
    carry_fill: usize,
    input: Vec<f32>,
    output: Vec<f32>,
    ring: [Vec<f32>; 2],
    write: usize,
    fill: usize,
}

impl Adapter {
    pub fn new(state: &State, lanes: usize, seeds: &[u64]) -> Result<Self> {
        let lanes = lanes.min(binaural::MAX_LANES);
        let seeds: Vec<u64> = (0..lanes)
            .map(|lane| {
                seeds
                    .get(lane)
                    .copied()
                    .unwrap_or(0x4d5241444d + lane as u64)
            })
            .collect();
        let dirs = state.directions.len();
        if state.hrir_len == 0
            || !(8..=200_000).contains(&dirs)
            || !dirs.is_multiple_of(2)
            || lanes == 0
            || dirs.checked_mul(state.hrir_len) != Some(state.hrirs.len())
        {
            return Err(Error {
                code: 1,
                message: "Invalid spreader creation arguments".to_owned(),
                location: Location::None,
            });
        }
        let spreader = Spreader::new(
            &state.hrirs,
            &state.directions,
            state.hrir_len,
            state.sample_rate,
            &seeds,
        )
        .map_err(|e| dsp(&e))?;
        Ok(Self {
            spreader,
            lanes,
            gains: [0.; binaural::MAX_LANES],
            carry: vec![0.; lanes * FRAME],
            carry_fill: 0,
            input: vec![0.; lanes * FRAME],
            output: vec![0.; 2 * FRAME],
            ring: [vec![0.; RING], vec![0.; RING]],
            write: 0,
            fill: 0,
        })
    }

    /// Direction, cone and gain of a lane; an out-of-range lane is ignored.
    pub fn set_source(
        &mut self,
        lane: usize,
        az: f32,
        el: f32,
        spread: f32,
        gain: f32,
    ) -> Result<()> {
        if lane >= self.lanes {
            return Ok(());
        }
        self.spreader
            .set_source(lane, az, el, spread)
            .map_err(|e| dsp(&e))?;
        self.gains[lane] = gain;
        Ok(())
    }

    fn push_batch(&mut self) -> Result<()> {
        let (lanes, _) = self.input.as_chunks_mut::<FRAME>();
        for (samples, g) in lanes.iter_mut().zip(&self.gains) {
            samples.iter_mut().for_each(|s| *s *= g);
        }
        self.spreader
            .process(&self.input, &mut self.output)
            .map_err(|e| dsp(&e))?;
        for f in 0..FRAME {
            let index = (self.write + f) % RING;
            self.ring[0][index] = self.output[f];
            self.ring[1][index] = self.output[FRAME + f];
        }
        self.write = (self.write + FRAME) % RING;
        self.fill += FRAME;
        Ok(())
    }

    /// Feeds `frames` of each lane's input (a missing lane is silence) and adds `frames` of output.
    pub fn process(
        &mut self,
        inputs: &[&[f32]],
        frames: usize,
        left: &mut [f32],
        right: &mut [f32],
    ) -> Result<()> {
        let mut position = 0;
        while position < frames {
            let fill = self.carry_fill;
            let n = (FRAME - fill).min(frames - position);
            let (lanes, _) = self.carry.as_chunks_mut::<FRAME>();
            for (lane, carry) in lanes.iter_mut().enumerate() {
                let carry = &mut carry[fill..fill + n];
                match inputs.get(lane) {
                    Some(input) => carry.copy_from_slice(&input[position..position + n]),
                    None => carry.fill(0.),
                }
            }
            self.carry_fill += n;
            position += n;
            if self.carry_fill < FRAME {
                break;
            }
            self.input.copy_from_slice(&self.carry);
            self.carry_fill = 0;
            self.push_batch()?;
        }
        let count = frames.min(self.fill);
        let start = (self.write + RING - self.fill) % RING;
        for f in 0..count {
            let index = (start + f) % RING;
            left[f] += self.ring[0][index];
            right[f] += self.ring[1][index];
        }
        self.fill -= count;
        Ok(())
    }

    /// One silent frame in the ring, so every later `process` drains exactly its frames.
    pub fn prime(&mut self) -> Result<()> {
        self.input.fill(0.);
        self.push_batch()
    }
}

// ── Per-source OLA ────────────────────────────────────────────────────────────

/// The quantized HRTF of a direction, with its checkpoints in diagnostic builds.
fn query(filters: &Filters, az: f32, el: f32, out: &mut Vec<f32>) -> Result<()> {
    if !az.is_finite() || !el.is_finite() {
        return Err(Error {
            code: 1,
            message: "Invalid HRTF query arguments".to_owned(),
            location: Location::None,
        });
    }
    out.resize(filters.output_len(), 0.);
    if !ENABLED {
        return filters
            .query(az, el, Lookup::Quantized, out, None)
            .map_err(|e| dsp(&e));
    }
    let n = out.len();
    let (mut magnitudes, mut sum, mut scales) = (vec![0.; n / 2 * 3], vec![0.; n], vec![0.; n]);
    filters
        .query(
            az,
            el,
            Lookup::Quantized,
            out,
            Some(Trace {
                magnitudes: &mut magnitudes,
                complex_sum: &mut sum,
                scales: &mut scales,
            }),
        )
        .map_err(|e| dsp(&e))?;
    let g = grid_index(az, el).map_err(|e| dsp(&e))?;
    let _ = process(&format!("hrtf-grid-{g}.c32"), Words::F32(out));
    let _ = process(
        &format!("hrtf-grid-{g}-magnitudes.f32"),
        Words::F32(&magnitudes),
    );
    let _ = process(&format!("hrtf-grid-{g}-complex-sum.c32"), Words::F32(&sum));
    let _ = process(&format!("hrtf-grid-{g}-scale.f32"), Words::F32(&scales));
    Ok(())
}

#[derive(Clone, Copy)]
struct BankShape {
    fft_size: usize,
    overlap: usize,
    block: usize,
}

/// One source's convolution history and buffers.
pub struct SourceState {
    convolver: OlaConvolver,
    diffuse: DiffuseDelay,
    cache: (f32, f32, Vec<f32>),
    end_hrtf: Vec<f32>,
    /// The last head-rotated direction of a steady block, while valid.
    head: Option<(f32, f32)>,
    pub left: Vec<f32>,
    pub right: Vec<f32>,
    ch_in: Vec<f32>,
    diffuse_in: Vec<f32>,
    error: Option<Error>,
}

impl SourceState {
    fn new(shape: BankShape) -> Result<Self> {
        let block = shape.block;
        Ok(Self {
            convolver: OlaConvolver::new(shape.fft_size, shape.overlap, block)
                .map_err(|e| dsp(&e))?,
            diffuse: DiffuseDelay::default(),
            cache: (f32::NAN, f32::NAN, Vec::new()),
            end_hrtf: Vec::new(),
            head: None,
            left: vec![0.; block],
            right: vec![0.; block],
            ch_in: vec![0.; block],
            diffuse_in: vec![0.; block],
            error: None,
        })
    }

    fn reset(&mut self) {
        self.convolver.reset();
        self.diffuse.reset();
        self.cache = (f32::NAN, f32::NAN, Vec::new());
        self.head = None;
    }

    fn cached(&mut self, filters: &Filters, az: f32, el: f32) -> Result<()> {
        let (caz, cel, hrtf) = &mut self.cache;
        if hrtf.is_empty() || az != *caz || el != *cel {
            query(filters, az, el, hrtf)?;
            (*caz, *cel) = (az, el);
        }
        Ok(())
    }
}

fn first_relevant(src: &Source, frame: u64) -> usize {
    src.blocks.iter().take_while(|b| b.end <= frame).count()
}

fn first_overlapping(src: &Source, start: u64, end: u64) -> Option<usize> {
    src.blocks
        .iter()
        .position(|b| b.end > start && b.start < end)
}

fn last_overlapping(src: &Source, start: u64, end: u64) -> Option<usize> {
    src.blocks
        .iter()
        .rposition(|b| b.end > start && b.start < end)
}

/// A single block covering `[start, end)` with no transition or gap.
fn steady_over(src: &Source, start: u64, end: u64) -> Option<usize> {
    let first = first_overlapping(src, start, end)?;
    let b = &src.blocks[first];
    (Some(first) == last_overlapping(src, start, end) && b.start <= start && b.end >= end)
        .then_some(first)
}

fn copy_windowed(src: &Source, input: &[f32], channels: usize, start: u64, out: &mut [f32]) {
    out.fill(0.);
    let mut bi = first_relevant(src, start);
    let ic = usize::from(src.channel);
    for (f, value) in out.iter_mut().enumerate() {
        let frame = start + f as u64;
        while bi < src.blocks.len() && src.blocks[bi].end <= frame {
            bi += 1;
        }
        if bi < src.blocks.len() && src.blocks[bi].start <= frame {
            *value = if ic < channels {
                input[f * channels + ic]
            } else {
                0.
            };
        }
    }
}

/// Renders one source's chunk `[start, start + frames)` into its `left` / `right`.
#[allow(clippy::too_many_arguments)]
fn render_source(
    src: &Source,
    filters: &Filters,
    st: &mut SourceState,
    input: &[f32],
    channels: usize,
    start: u64,
    frames: usize,
    smoothing: u32,
    head: Option<&Rotation>,
    live_head_locked: Option<bool>,
) -> Result<()> {
    if src.blocks.is_empty() {
        return Ok(());
    }
    st.left[..frames].fill(0.);
    st.right[..frames].fill(0.);
    let end = start + frames as u64;
    // An explicit live value wins; otherwise the block's effective headLocked.
    let tracks_head =
        |b: &binaural::Block| head.is_some() && !live_head_locked.unwrap_or(b.head_locked);
    let direction = |b: &binaural::Block, az: f32, el: f32| -> (f32, f32) {
        match head {
            Some(r) if tracks_head(b) => {
                let [a, e] = r.apply(az, el, false);
                (a, e)
            }
            _ => (az, el),
        }
    };
    let ic = usize::from(src.channel);
    // A steady head-tracked block crossfades the kernel when the rotated direction changed.
    let steady = steady_over(src, start, end);
    if let (false, Some(i), Some(r)) = (src.bypass_lfe, steady, head)
        && tracks_head(&src.blocks[i])
    {
        let b = src.blocks[i];
        let [cur_az, cur_el] = r.apply(b.az, b.el, false);
        copy_windowed(src, input, channels, start, &mut st.ch_in[..frames]);
        let diffuse_bus = src.diffuse_bus;
        if diffuse_bus {
            st.diffuse
                .process(&st.ch_in[..frames], &mut st.diffuse_in[..frames])
                .map_err(|e| dsp(&e))?;
        }
        let gain = src.gain * b.gain;
        match st.head {
            Some((paz, pel)) if cur_az != paz || cur_el != pel => {
                st.cached(filters, paz, pel)?;
                query(filters, cur_az, cur_el, &mut st.end_hrtf)?;
                let conv_in = if diffuse_bus {
                    &st.diffuse_in
                } else {
                    &st.ch_in
                };
                st.convolver
                    .process(
                        &conv_in[..frames],
                        &st.cache.2,
                        gain,
                        Some((&st.end_hrtf, gain)),
                        &mut st.left[..frames],
                        &mut st.right[..frames],
                    )
                    .map_err(|e| dsp(&e))?;
            }
            _ => {
                st.cached(filters, cur_az, cur_el)?;
                let conv_in = if diffuse_bus {
                    &st.diffuse_in
                } else {
                    &st.ch_in
                };
                st.convolver
                    .process(
                        &conv_in[..frames],
                        &st.cache.2,
                        gain,
                        None,
                        &mut st.left[..frames],
                        &mut st.right[..frames],
                    )
                    .map_err(|e| dsp(&e))?;
            }
        }
        st.head = Some((cur_az, cur_el));
        return Ok(());
    }
    // Tracking inactive or head-locked, or a block boundary or gap: re-seed later.
    st.head = None;

    if smoothing > 0 && src.smoothable && !src.bypass_lfe {
        let (first, last) = (
            first_overlapping(src, start, end),
            last_overlapping(src, start, end),
        );
        if let (Some(a), Some(z)) = (first, last)
            && a != z
            && !src.blocks[z].jump
        {
            let (sb, eb) = (src.blocks[a], src.blocks[z]);
            if eb.interpolation.unwrap_or(u64::from(smoothing)) > 0 {
                copy_windowed(src, input, channels, start, &mut st.ch_in[..frames]);
                if src.diffuse_bus {
                    st.diffuse
                        .process(&st.ch_in[..frames], &mut st.diffuse_in[..frames])
                        .map_err(|e| dsp(&e))?;
                }
                let (s_az, s_el) = direction(&sb, sb.az, sb.el);
                let (e_az, e_el) = direction(&eb, eb.az, eb.el);
                st.cached(filters, s_az, s_el)?;
                query(filters, e_az, e_el, &mut st.end_hrtf)?;
                let conv_in = if src.diffuse_bus {
                    &st.diffuse_in
                } else {
                    &st.ch_in
                };
                return st
                    .convolver
                    .process(
                        &conv_in[..frames],
                        &st.cache.2,
                        src.gain * sb.gain,
                        Some((&st.end_hrtf, src.gain * eb.gain)),
                        &mut st.left[..frames],
                        &mut st.right[..frames],
                    )
                    .map_err(|e| dsp(&e));
            }
        }
    }

    let mut cursor = start;
    let mut bi = first_relevant(src, start);
    while cursor < end {
        while bi < src.blocks.len() && src.blocks[bi].end <= cursor {
            bi += 1;
        }
        let off = (cursor - start) as usize;
        if bi >= src.blocks.len() || src.blocks[bi].start >= end {
            st.convolver
                .advance_silence(&mut st.left[off..frames], &mut st.right[off..frames])
                .map_err(|e| dsp(&e))?;
            break;
        }
        let b = src.blocks[bi];
        if cursor < b.start {
            let silent_end = b.start.min(end);
            let n = (silent_end - cursor) as usize;
            st.convolver
                .advance_silence(&mut st.left[off..off + n], &mut st.right[off..off + n])
                .map_err(|e| dsp(&e))?;
            cursor = silent_end;
            continue;
        }
        let seg_end = b.end.min(end);
        if seg_end <= cursor {
            bi += 1;
            continue;
        }
        let n = (seg_end - cursor) as usize;
        for f in 0..n {
            st.ch_in[f] = if ic < channels {
                input[(off + f) * channels + ic]
            } else {
                0.
            };
        }
        let gain = src.gain * b.gain;
        if src.bypass_lfe {
            for f in 0..n {
                let sample = st.ch_in[f] * gain;
                st.left[off + f] += sample;
                st.right[off + f] += sample;
            }
        } else {
            if src.diffuse_bus {
                st.diffuse
                    .process(&st.ch_in[..n], &mut st.diffuse_in[..n])
                    .map_err(|e| dsp(&e))?;
            }
            let (h_az, h_el) = direction(&b, b.az, b.el);
            st.cached(filters, h_az, h_el)?;
            let conv_in = if src.diffuse_bus {
                &st.diffuse_in
            } else {
                &st.ch_in
            };
            st.convolver
                .process(
                    &conv_in[..n],
                    &st.cache.2,
                    gain,
                    None,
                    &mut st.left[off..off + n],
                    &mut st.right[off..off + n],
                )
                .map_err(|e| dsp(&e))?;
        }
        cursor = seg_end;
        if cursor >= b.end {
            bi += 1;
        }
    }
    Ok(())
}

/// Worker threads for `count` tasks: the C++ default (none for a single task or core), or
/// `MR_ADM_DIAGNOSTIC_WORKERS` in diagnostic builds.
pub fn workers(count: usize) -> std::result::Result<usize, String> {
    let hardware = std::thread::available_parallelism().map_or(1, usize::from);
    binaural::diagnostic_count(
        "MR_ADM_DIAGNOSTIC_WORKERS",
        if count > 1 && hardware > 1 {
            count.min(hardware)
        } else {
            0
        },
    )
}

fn pool(threads: usize) -> Result<Option<rayon::ThreadPool>> {
    if threads == 0 {
        return Ok(None);
    }
    rayon::ThreadPoolBuilder::new()
        .num_threads(threads)
        .thread_name(|i| format!("mradm-binaural-{i}"))
        .build()
        .map(Some)
        .map_err(|_| Error {
            code: 4,
            message: "Cannot start binaural workers".to_owned(),
            location: Location::None,
        })
}

/// Runs `f` over `items` on the pool (or serially).
fn each<T: Send>(
    pool: &Option<rayon::ThreadPool>,
    items: &mut [T],
    f: impl Fn(usize, &mut T) + Sync,
) {
    match pool {
        Some(p) if items.len() > 1 => p.install(|| {
            items
                .par_iter_mut()
                .enumerate()
                .for_each(|(i, item)| f(i, item));
        }),
        _ => items
            .iter_mut()
            .enumerate()
            .for_each(|(i, item)| f(i, item)),
    }
}

/// The OLA state of a source list.
pub struct Bank {
    pub states: Vec<SourceState>,
    pool: Option<rayon::ThreadPool>,
    shape: BankShape,
}

impl Bank {
    pub fn new(state: &State, sources: usize, block: usize, threads: usize) -> Result<Self> {
        let shape = BankShape {
            fft_size: state.fft_size,
            overlap: state.overlap,
            block,
        };
        Ok(Self {
            states: (0..sources)
                .map(|_| SourceState::new(shape))
                .collect::<Result<_>>()?,
            pool: pool(threads)?,
            shape,
        })
    }

    /// Reuses this stream's workers and convolution storage for a new source graph. Prepare
    /// any additional sources before resetting history, so a preparation error leaves the old
    /// graph's state intact. Removed sources release their convolution storage immediately.
    pub fn rebuild(&mut self, sources: usize) -> Result<()> {
        let additional = sources.saturating_sub(self.states.len());
        let added = (0..additional)
            .map(|_| SourceState::new(self.shape))
            .collect::<Result<Vec<_>>>()?;
        self.states.reserve(additional);
        self.states.truncate(sources);
        self.states.extend(added);
        for state in &mut self.states {
            state.reset();
            state.left.fill(0.);
            state.right.fill(0.);
            state.end_hrtf.clear();
            state.error = None;
        }
        Ok(())
    }

    pub fn reset(&mut self) {
        self.states.iter_mut().for_each(SourceState::reset);
    }

    /// Renders every source's chunk; the first failing source (by index) reports.
    #[allow(clippy::too_many_arguments)]
    pub fn render(
        &mut self,
        sources: &[Source],
        filters: &Filters,
        input: &[f32],
        channels: usize,
        start: u64,
        frames: usize,
        smoothing: u32,
        head: Option<&Rotation>,
        head_locked: &(dyn Fn(usize) -> Option<bool> + Sync),
    ) -> Result<()> {
        each(&self.pool, &mut self.states, |i, st| {
            st.error = render_source(
                &sources[i],
                filters,
                st,
                input,
                channels,
                start,
                frames,
                smoothing,
                head,
                head_locked(i),
            )
            .err();
        });
        match self.states.iter_mut().find_map(|s| s.error.take()) {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }
}

// ── Offline driver ────────────────────────────────────────────────────────────

pub struct Settings {
    pub inputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub window: Option<Window>,
    pub smoothing_frames: u32,
    /// The static listener orientation (yaw, pitch, roll); `None` for the identity.
    pub pose: Option<[f32; 3]>,
}

/// One step's output: `emit` interleaved stereo frames to write.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Step {
    pub emit: usize,
    pub tail: bool,
    pub progress: Option<Progress>,
}

/// What the next `next_step` renders.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pending {
    Done,
    Block,
    Tail,
}

struct GroupState {
    adapter: Adapter,
    left: Vec<f32>,
    right: Vec<f32>,
    tracks: Vec<f32>,
    zeros: Vec<f32>,
    error: Option<Error>,
}

/// The offline render loop, stepped one block at a time; the caller writes each step's frames
/// and owns cancellation (before each input block) and progress.
pub struct Offline {
    prepared: Arc<Prepared>,
    reader: Reader<BufReader<File>>,
    channels: usize,
    frames: u64,
    block: u64,
    smoothing: u32,
    bank: Bank,
    groups: Vec<GroupState>,
    group_pool: Option<rayon::ThreadPool>,
    rotation: Option<Rotation>,
    meter: Meter,
    input: Vec<f32>,
    left: Vec<f32>,
    right: Vec<f32>,
    ola: [Vec<f32>; 2],
    delay: [Vec<f32>; 2],
    delay_pos: usize,
    spreader_delay: usize,
    out_skip: usize,
    out_abs: u64,
    out_written: u64,
    output: Vec<f32>,
    win_start: u64,
    win_end: u64,
    start_pos: u64,
    done: u64,
    tail_remaining: usize,
    diagnostic_pools: Option<String>,
}

/// The aligned pre-roll start: enough whole blocks before the target block to converge the OLA
/// overlap and the diffuse delay.
pub fn preroll_start(target: u64, block: u64, overlap: u64) -> u64 {
    let warmup = block.max(overlap).max(DELAY_LENGTH as u64);
    let blocks = warmup.div_ceil(block);
    let target_block = target / block;
    if target_block > blocks {
        (target_block - blocks) * block
    } else {
        0
    }
}

impl Offline {
    pub fn open_reader(path: &str) -> Result<Reader<BufReader<File>>> {
        crate::offline::Driver::open_reader(path)
    }

    pub fn new(
        prepared: Arc<Prepared>,
        reader: Reader<BufReader<File>>,
        settings: Settings,
    ) -> Result<Self> {
        let state = Arc::clone(&prepared.hrtf);
        let mut groups = Vec::with_capacity(prepared.groups.len());
        let block = prepared.block;
        let fn_max = block as usize;
        if prepared.spread == Spread::SafSpreader {
            for group in &prepared.groups {
                let seeds: Vec<u64> = group
                    .lanes
                    .iter()
                    .map(|lane| {
                        prepared.tracks[group.tracks[lane.track_slot]].seed
                            ^ ((lane.source as u64 + 1).wrapping_mul(0x9e3779b97f4a7c15))
                    })
                    .collect();
                let mut adapter = Adapter::new(&state, group.lanes.len(), &seeds)?;
                adapter.prime()?;
                groups.push(GroupState {
                    adapter,
                    left: vec![0.; fn_max],
                    right: vec![0.; fn_max],
                    tracks: vec![0.; group.tracks.len() * fn_max],
                    zeros: vec![0.; fn_max],
                    error: None,
                });
            }
        }
        let meter = Meter::new(2, settings.sample_rate, MeterMode::IntegratedTruePeak, &[])
            .map_err(|e| Error::dsp(&e, Location::Meter))?;
        let worker_error = |message: String| Error {
            code: 4,
            message,
            location: Location::None,
        };
        let spreader_mode = !groups.is_empty();
        let spreader_delay = if spreader_mode { total_latency() } else { 0 };
        let spreader_workers = workers(prepared.groups.len()).map_err(worker_error)?;
        let ola_workers = workers(prepared.sources.len()).map_err(worker_error)?;
        let diagnostic_pools = ENABLED.then(|| {
            format!(
                "diagnostic pools: ola_workers={ola_workers} spreader_workers={spreader_workers} \
                 ola_sources={} spreader_groups={}",
                prepared.sources.len(),
                prepared.groups.len()
            )
        });
        let group_pool = pool(if spreader_mode { spreader_workers } else { 0 })?;
        let bank = Bank::new(&state, prepared.sources.len(), fn_max, ola_workers)?;
        let frames = settings.frames;
        let (win_start, win_end) = match settings.window {
            Some(w) => {
                let start = w.start.min(frames);
                (start, start.saturating_add(w.count).min(frames))
            }
            None => (0, frames),
        };
        let start_pos = if settings.window.is_some() && !spreader_mode && win_start > 0 {
            preroll_start(win_start, block, state.overlap as u64)
        } else {
            0
        };
        let mut reader = reader;
        if start_pos > 0 {
            reader.seek_frame(start_pos).map_err(Error::wav)?;
        }
        let rotation = match settings.pose {
            Some(pose) => Some(Rotation::new_compat(pose, false).map_err(|e| dsp(&e))?),
            None => None,
        };
        Ok(Self {
            channels: settings.inputs,
            frames,
            block,
            smoothing: settings.smoothing_frames,
            input: vec![0.; settings.inputs * fn_max],
            left: vec![0.; fn_max],
            right: vec![0.; fn_max],
            ola: [vec![0.; fn_max], vec![0.; fn_max]],
            delay: [vec![0.; spreader_delay], vec![0.; spreader_delay]],
            delay_pos: 0,
            spreader_delay,
            out_skip: spreader_delay,
            out_abs: start_pos,
            out_written: 0,
            output: vec![0.; 2 * fn_max],
            win_start,
            win_end,
            start_pos,
            done: start_pos,
            tail_remaining: spreader_delay,
            prepared,
            reader,
            bank,
            groups,
            group_pool,
            rotation,
            meter,
            diagnostic_pools,
        })
    }

    /// The diagnostic build's pool log line.
    pub fn diagnostic_pools(&self) -> Option<&str> {
        self.diagnostic_pools.as_deref()
    }

    pub fn pending(&self) -> Pending {
        if self.done < self.frames && self.out_abs < self.win_end {
            Pending::Block
        } else if self.spreader_delay > 0 && self.tail_remaining > 0 && self.out_abs < self.win_end
        {
            Pending::Tail
        } else {
            Pending::Done
        }
    }

    /// The frames of the last step to write, interleaved.
    pub fn output(&self, emit: usize) -> &[f32] {
        &self.output[..emit * 2]
    }

    /// Skips the latency, clips to the window, interleaves and meters; returns the frames to write.
    fn emit(&mut self, count: usize) -> Result<usize> {
        let mut local = 0;
        if self.out_skip > 0 {
            let s = self.out_skip.min(count);
            self.out_skip -= s;
            local = s;
        }
        let available = count - local;
        if available == 0 {
            return Ok(0);
        }
        let base = self.out_abs;
        self.out_abs += available as u64;
        let lo = base.max(self.win_start);
        let hi = (base + available as u64).min(self.win_end);
        if hi <= lo {
            return Ok(0);
        }
        let offset = local + (lo - base) as usize;
        let want = (hi - lo) as usize;
        for f in 0..want {
            self.output[f * 2] = self.left[offset + f];
            self.output[f * 2 + 1] = self.right[offset + f];
        }
        self.meter
            .add_frames(&self.output[..want * 2])
            .map_err(|e| Error::dsp(&e, Location::Meter))?;
        self.out_written += want as u64;
        Ok(want)
    }

    pub fn next_step(&mut self) -> Result<Option<Step>> {
        match self.pending() {
            Pending::Done => Ok(None),
            Pending::Block => self.block_step().map(Some),
            Pending::Tail => self.tail_step().map(Some),
        }
    }

    fn block_step(&mut self) -> Result<Step> {
        let fnow = self.block.min(self.frames - self.done) as usize;
        let channels = self.channels;
        let input = &mut self.input[..channels * fnow];
        let read = self.reader.read_frames(input).map_err(Error::wav)?;
        if read != fnow as u64 {
            return Err(Error {
                code: 3,
                message: "short render input read".to_owned(),
                location: Location::None,
            });
        }
        self.left[..fnow].fill(0.);
        self.right[..fnow].fill(0.);
        let spreader_mode = self.spreader_delay > 0;
        let prepared = Arc::clone(&self.prepared);
        self.bank.render(
            &prepared.sources,
            &prepared.hrtf.filters,
            &self.input[..channels * fnow],
            channels,
            self.done,
            fnow,
            self.smoothing,
            self.rotation.as_ref(),
            &|_| None,
        )?;
        {
            let [ola_l, ola_r] = &mut self.ola;
            let (dl, dr) = if spreader_mode {
                ola_l[..fnow].fill(0.);
                ola_r[..fnow].fill(0.);
                (&mut ola_l[..fnow], &mut ola_r[..fnow])
            } else {
                (&mut self.left[..fnow], &mut self.right[..fnow])
            };
            for st in &self.bank.states {
                for f in 0..fnow {
                    dl[f] += st.left[f];
                    dr[f] += st.right[f];
                }
            }
        }
        if spreader_mode {
            let [delay_l, delay_r] = &mut self.delay;
            for f in 0..fnow {
                self.left[f] += delay_l[self.delay_pos];
                self.right[f] += delay_r[self.delay_pos];
                delay_l[self.delay_pos] = self.ola[0][f];
                delay_r[self.delay_pos] = self.ola[1][f];
                self.delay_pos = (self.delay_pos + 1) % self.spreader_delay;
            }
            self.spreader_block(fnow)?;
        }
        let emit = self.emit(fnow)?;
        self.done += fnow as u64;
        let done = self.done.min(self.win_end) - self.start_pos;
        let total = (self.win_end - self.start_pos).max(1);
        let stage_fraction = done as f64 / total as f64;
        Ok(Step {
            emit,
            tail: false,
            progress: Some(Progress {
                fraction: 0.2 + 0.7 * stage_fraction,
                stage_fraction,
                done,
                total,
            }),
        })
    }

    fn spreader_block(&mut self, fnow: usize) -> Result<()> {
        let prepared = Arc::clone(&self.prepared);
        let (start, end) = (self.done, self.done + fnow as u64);
        let channels = self.channels;
        let input = &self.input[..channels * fnow];
        let rotation = self.rotation.as_ref();
        each(&self.group_pool, &mut self.groups, |gi, g| {
            g.error = spreader_group(
                &prepared, gi, g, input, channels, start, end, fnow, rotation,
            )
            .err();
        });
        if let Some(e) = self.groups.iter_mut().find_map(|g| g.error.take()) {
            return Err(e);
        }
        for g in &self.groups {
            for f in 0..fnow {
                self.left[f] += g.left[f];
                self.right[f] += g.right[f];
            }
        }
        Ok(())
    }

    fn tail_step(&mut self) -> Result<Step> {
        let tn = (self.block as usize).min(self.tail_remaining);
        self.left[..tn].fill(0.);
        self.right[..tn].fill(0.);
        let [delay_l, delay_r] = &mut self.delay;
        for f in 0..tn {
            self.left[f] += delay_l[self.delay_pos];
            self.right[f] += delay_r[self.delay_pos];
            delay_l[self.delay_pos] = 0.;
            delay_r[self.delay_pos] = 0.;
            self.delay_pos = (self.delay_pos + 1) % self.spreader_delay;
        }
        for g in &mut self.groups {
            let zeros = &g.zeros[..tn];
            let lanes = vec![zeros; g.adapter.lanes];
            g.adapter
                .process(&lanes, tn, &mut self.left[..tn], &mut self.right[..tn])?;
        }
        let emit = self.emit(tn)?;
        self.tail_remaining -= tn;
        Ok(Step {
            emit,
            tail: true,
            progress: None,
        })
    }

    /// The frames written, the integrated loudness and the true peak (with the -200 dB floor).
    pub fn finish(self) -> (u64, Metrics) {
        let lufs = self.meter.integrated().ok().filter(|v| v.is_finite());
        let peak_dbtp = self
            .meter
            .max_true_peak()
            .ok()
            .map(|p| mradm_dsp::post_gain::amplitude_to_db(p.max(1e-10)));
        (self.out_written, Metrics { lufs, peak_dbtp })
    }
}

/// The block in effect at `cursor`, narrowing `next` to its end or the next start.
fn active_block(track: &Track, cursor: u64, chunk_end: u64, next: &mut u64) -> Option<usize> {
    for (i, b) in track.blocks.iter().enumerate() {
        if b.end <= cursor {
            continue;
        }
        if b.start <= cursor {
            *next = (*next).min(b.end).min(chunk_end);
            return Some(i);
        }
        *next = (*next).min(b.start).min(chunk_end);
        return None;
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn spreader_group(
    prepared: &Prepared,
    gi: usize,
    g: &mut GroupState,
    input: &[f32],
    channels: usize,
    start: u64,
    end: u64,
    fnow: usize,
    rotation: Option<&Rotation>,
) -> Result<()> {
    let group = &prepared.groups[gi];
    g.left[..fnow].fill(0.);
    g.right[..fnow].fill(0.);
    g.tracks.fill(0.);
    let mut active = vec![None; group.tracks.len()];
    let mut cursor = start;
    while cursor < end {
        let mut next = end;
        for (ts, a) in active.iter_mut().enumerate() {
            *a = active_block(&prepared.tracks[group.tracks[ts]], cursor, end, &mut next);
        }
        let seg_off = (cursor - start) as usize;
        let seg = (next - cursor) as usize;
        if seg == 0 {
            break;
        }
        for (ts, a) in active.iter().enumerate() {
            if a.is_none() {
                continue;
            }
            let ic = usize::from(prepared.tracks[group.tracks[ts]].channel);
            let buffer = &mut g.tracks[ts * fnow..ts * fnow + seg];
            for (f, v) in buffer.iter_mut().enumerate() {
                *v = if ic < channels {
                    input[(seg_off + f) * channels + ic]
                } else {
                    0.
                };
            }
        }
        // (track slot) of each lane's input, or None for silence.
        let mut lane_tracks: Vec<Option<usize>> = vec![None; group.lanes.len()];
        for (li, lane) in group.lanes.iter().enumerate() {
            let Some(b) = active[lane.track_slot] else {
                continue;
            };
            let track = &prepared.tracks[group.tracks[lane.track_slot]];
            let block = &track.blocks[b];
            let Some(ss) = block.sources.get(lane.source) else {
                continue;
            };
            // The cone centre follows the static listener pose; the half-angle does not.
            let (az, el) = match rotation {
                Some(r) if !block.head_locked => {
                    let [a, e] = r.apply(ss.az, ss.el, false);
                    (a, e)
                }
                _ => (ss.az, ss.el),
            };
            let gain = track.object_gain * ss.gain;
            g.adapter.set_source(li, az, el, ss.spread, gain)?;
            if ENABLED {
                let _ = process(
                    &format!("spreader/s{cursor}-g{gi}-lane{li}.20-config.f32"),
                    Words::F32(&[az, el, ss.spread, gain]),
                );
            }
            lane_tracks[li] = Some(lane.track_slot);
        }
        let lanes: Vec<&[f32]> = lane_tracks
            .iter()
            .map(|t| match t {
                Some(ts) => &g.tracks[ts * fnow..ts * fnow + seg],
                None => &g.zeros[..seg],
            })
            .collect();
        let key = format!("spreader/s{cursor}-g{gi}");
        if ENABLED {
            for (li, samples) in lanes.iter().enumerate() {
                let _ = process(&format!("{key}-lane{li}.30-input.f32"), Words::F32(samples));
            }
        }
        g.adapter.process(
            &lanes,
            seg,
            &mut g.left[seg_off..seg_off + seg],
            &mut g.right[seg_off..seg_off + seg],
        )?;
        if ENABLED {
            let _ = process(
                &format!("{key}.50-left.f32"),
                Words::F32(&g.left[seg_off..seg_off + seg]),
            );
            let _ = process(
                &format!("{key}.50-right.f32"),
                Words::F32(&g.right[seg_off..seg_off + seg]),
            );
        }
        cursor = next;
    }
    Ok(())
}
