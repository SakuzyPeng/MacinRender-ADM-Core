//! Shared numeric preparation, bounded dynamic curves and per-render size state.
#![allow(clippy::needless_range_loop)]
use super::{
    Event, Layout, Position, count, invalid, panner,
    processor::{Processor, State, Track},
};
use crate::{
    Error, Result,
    pcm_mix::{self, Block, Coefficients, Row},
};
use std::sync::Arc;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TbRowInput {
    pub input: usize,
    pub event_offset: usize,
    pub event_count: usize,
    pub bed_offset: usize,
    pub size_index: usize,
    pub kind: u32,
    pub gain: f32,
}
struct SizedTrack {
    track: Arc<Track>,
    input: usize,
    gain: f32,
    initial: panner::Gains,
}
pub struct Plan {
    pub mix: Arc<pcm_mix::Plan>,
    tracks: Vec<SizedTrack>,
    inputs: usize,
    layout: Layout,
}
fn motion(
    events: &[Event],
    layout: Layout,
    total: u64,
    blocks: &mut Vec<Block>,
    gains: &mut Vec<f32>,
) -> Result<()> {
    let first = events.first().ok_or_else(invalid)?;
    if first.start != 0 {
        return Err(invalid());
    }
    if events.windows(2).any(|pair| pair[1].start < pair[0].start) {
        return Err(Error::Unsupported(
            "Triple Balance requires nondecreasing metadata timestamps",
        ));
    }
    if events.last().is_some_and(|e| e.start >= total) {
        return Err(Error::Unsupported(
            "Triple Balance metadata lies outside the timeline",
        ));
    }
    // Validate every source target, including updates superseded within a control block.
    for event in events {
        panner::point(event.position, 1., layout)?;
    }
    let mut next = events.partition_point(|e| e.start < 512);
    let first = &events[next - 1];
    let mut current = panner::point(first.position, 1., layout)?;
    blocks.push(Block {
        start: 0,
        end: u64::MAX,
        interpolation: 0,
        flags: 3,
    });
    gains.extend_from_slice(&current[..layout.channels()]);
    if next == events.len() {
        return Ok(());
    }
    let mut state = first.position.internal();
    let mut target = state;
    let mut control = 512u64;
    while control < total {
        let end = control.checked_add(512).ok_or_else(invalid)?;
        while next < events.len() && events[next].start < end {
            target = events[next].position.internal();
            next += 1;
        }
        state.advance(target);
        let values = panner::point(state.adm(), 1., layout)?;
        if values != current {
            current = values;
            blocks.push(Block {
                start: control,
                end: u64::MAX,
                interpolation: 512,
                flags: 6,
            });
            gains.extend_from_slice(&current[..layout.channels()]);
        }
        control = end;
    }
    if next != events.len() {
        return Err(Error::Unsupported(
            "Triple Balance metadata lies outside the timeline",
        ));
    }
    Ok(())
}
impl Plan {
    pub fn new(
        inputs: usize,
        layout: Layout,
        rate: u32,
        total: u64,
        rows: &[TbRowInput],
        events: &[Event],
        bed: &[f32],
    ) -> Result<Self> {
        if inputs == 0 || rate == 0 || (layout == Layout::Room222 && rate != 48000) {
            return Err(invalid());
        }
        let outputs = layout.channels();
        let size_count = rows.iter().filter(|r| r.kind == 2).count();
        let mut sizes: Vec<Option<SizedTrack>> = (0..size_count).map(|_| None).collect();
        let mut mix_rows = Vec::with_capacity(rows.len());
        let mut blocks = Vec::new();
        let mut gains = Vec::new();
        for r in rows {
            if r.input >= inputs
                || !r.gain.is_finite()
                || r.kind > 2
                || r.event_offset > events.len()
                || r.event_count > events.len() - r.event_offset
            {
                return Err(invalid());
            }
            let start = blocks.len();
            let source = &events[r.event_offset..r.event_offset + r.event_count];
            if r.kind == 0 {
                if r.bed_offset > bed.len() || outputs > bed.len() - r.bed_offset {
                    return Err(invalid());
                }
                blocks.push(Block {
                    start: 0,
                    end: total,
                    interpolation: 0,
                    flags: 1,
                });
                gains.extend_from_slice(&bed[r.bed_offset..r.bed_offset + outputs]);
            } else if r.kind == 1 {
                motion(source, layout, total, &mut blocks, &mut gains)?;
            } else {
                if r.size_index >= size_count || sizes[r.size_index].is_some() {
                    return Err(invalid());
                }
                if source.last().is_none_or(|e| e.start >= total) {
                    return Err(invalid());
                }
                let converted = source
                    .iter()
                    .map(|e| Event {
                        position: e.position.internal(),
                        ..*e
                    })
                    .collect();
                let track = Arc::new(Track::new(converted, layout, rate)?);
                let (next, _) = track.initial_event();
                let initial = panner::point(source[next - 1].position, 1., layout)?;
                sizes[r.size_index] = Some(SizedTrack {
                    track,
                    input: r.input,
                    gain: r.gain,
                    initial,
                });
            }
            mix_rows.push(Row {
                input_channel: r.input,
                block_offset: start,
                block_count: blocks.len() - start,
                output_gain: r.gain,
            });
        }
        let mix = Arc::new(pcm_mix::Plan::new(
            inputs,
            outputs,
            mix_rows,
            blocks,
            Coefficients::Speaker(gains),
        )?);
        Ok(Self {
            mix,
            tracks: sizes.into_iter().map(Option::unwrap).collect(),
            inputs,
            layout,
        })
    }
    pub fn size_count(&self) -> usize {
        self.tracks.len()
    }
}
#[derive(Clone)]
struct Point {
    position: Position,
    target: Position,
    gains: panner::Gains,
    next: usize,
    control: u64,
}
pub struct Session {
    plan: Arc<Plan>,
    processors: Vec<Processor>,
    initial: Vec<Point>,
    points: Vec<Point>,
    curve_start: Vec<Point>,
    window: Option<(u64, usize)>,
    interpolation: u64,
    dynamic: Option<pcm_mix::Mixer>,
    blocks: Vec<Block>,
    gains: Vec<f32>,
    input: Vec<f32>,
    output: Vec<f32>,
    point_output: Vec<f32>,
    weights: Vec<f32>,
    targets: Vec<f32>,
    in_matrix: Vec<bool>,
    max_frames: usize,
}
pub struct Snapshot {
    plan: Arc<Plan>,
    states: Vec<State>,
    points: Vec<Point>,
    curve_start: Vec<Point>,
    window: Option<(u64, usize)>,
    max_frames: usize,
    interpolation: u64,
    weights: Vec<f32>,
    targets: Vec<f32>,
    in_matrix: Vec<bool>,
    live: bool,
}
impl Session {
    pub fn new(plan: Arc<Plan>, max_frames: usize, interpolation: u64, live: bool) -> Result<Self> {
        if max_frames == 0 {
            return Err(invalid());
        }
        count(max_frames, plan.inputs)?;
        let samples = count(max_frames, plan.layout.channels())?;
        let capacity = max_frames
            .div_ceil(512)
            .checked_add(1)
            .ok_or_else(invalid)?;
        let processors = plan
            .tracks
            .iter()
            .map(|t| Processor::new(Arc::clone(&t.track)))
            .collect();
        let initial: Vec<_> = if live {
            plan.tracks
                .iter()
                .map(|t| {
                    let (next, event) = t.track.initial_event();
                    Point {
                        position: event.position,
                        target: event.position,
                        gains: t.initial,
                        next,
                        control: 0,
                    }
                })
                .collect()
        } else {
            Vec::new()
        };
        let dynamic = if live {
            Some(pcm_mix::Mixer::dynamic(
                plan.inputs,
                plan.layout.channels(),
                &plan.tracks.iter().map(|t| t.input).collect::<Vec<_>>(),
                capacity,
                max_frames,
                interpolation,
            )?)
        } else {
            None
        };
        let n = plan.tracks.len();
        Ok(Self {
            processors,
            points: initial.clone(),
            curve_start: initial.clone(),
            window: None,
            interpolation,
            initial,
            dynamic,
            blocks: Vec::with_capacity(capacity),
            gains: Vec::with_capacity(count(capacity, plan.layout.channels())?),
            input: vec![0.; max_frames],
            output: vec![0.; samples],
            point_output: vec![0.; samples],
            weights: vec![1.; n],
            targets: vec![1.; n],
            in_matrix: vec![false; n],
            max_frames,
            plan,
        })
    }
    pub fn inputs(&self) -> usize {
        self.plan.inputs
    }
    pub fn outputs(&self) -> usize {
        self.plan.layout.channels()
    }
    pub fn size_count(&self) -> usize {
        self.processors.len()
    }
    pub fn reset(&mut self) {
        for p in &mut self.processors {
            p.reset();
        }
        self.weights.copy_from_slice(&self.targets);
        self.points.clone_from_slice(&self.initial);
        self.curve_start.clone_from_slice(&self.initial);
        self.window = None;
        if let Some(m) = &mut self.dynamic {
            m.reset();
        }
        self.input.fill(0.);
        self.output.fill(0.);
        self.point_output.fill(0.);
        self.blocks.clear();
        self.gains.clear();
    }
    fn scales_valid(&self, scales: &[f32]) -> Result<()> {
        if scales.len() != self.size_count() || scales.iter().any(|s| !s.is_finite() || *s < 0.) {
            return Err(invalid());
        }
        Ok(())
    }
    pub fn set_scales(&mut self, scales: &[f32], immediate: bool) -> Result<()> {
        self.scales_valid(scales)?;
        for (i, &scale) in scales.iter().enumerate() {
            self.processors[i].set_scale_validated(scale);
            self.targets[i] = if scale == 0. { 0. } else { 1. };
            if immediate {
                self.weights[i] = self.targets[i];
            }
        }
        Ok(())
    }
    pub fn set_matrix(&mut self, scales: &[f32]) -> Result<()> {
        self.scales_valid(scales)?;
        if self.dynamic.is_none() && scales.contains(&0.) {
            return Err(invalid());
        }
        for (i, &s) in scales.iter().enumerate() {
            self.in_matrix[i] = s == 0.;
        }
        Ok(())
    }
    pub fn prepare_points(&mut self, start: u64, frames: usize) -> Result<()> {
        let advance = count(frames.div_ceil(512), 512)? as u64;
        if frames > self.max_frames
            || start.checked_add(advance).is_none()
            || (!self.points.is_empty() && self.points.iter().any(|p| p.control != start))
        {
            return Err(invalid());
        }
        if frames == 0 {
            return Ok(());
        }
        let Some(m) = &mut self.dynamic else {
            return Ok(());
        };
        let channels = self.plan.layout.channels();
        self.curve_start.clone_from_slice(&self.points);
        self.window = Some((start, frames));
        for (i, state) in self.points.iter_mut().enumerate() {
            self.blocks.clear();
            self.gains.clear();
            self.blocks.push(Block {
                start: 0,
                end: u64::MAX,
                interpolation: 0,
                flags: 3,
            });
            self.gains.extend_from_slice(&state.gains[..channels]);
            let events = &self.plan.tracks[i].track.events;
            for _ in (0..frames).step_by(512) {
                if state.control != 0 && events.len() > 1 {
                    while state.next < events.len()
                        && events[state.next].start < state.control + 512
                    {
                        state.target = events[state.next].position;
                        state.next += 1;
                    }
                    state.position.advance(state.target);
                    let g = panner::point(state.position.adm(), 1., self.plan.layout)
                        .expect("validated dynamic position");
                    if g != state.gains {
                        state.gains = g;
                        self.blocks.push(Block {
                            start: state.control,
                            end: u64::MAX,
                            interpolation: 512,
                            flags: 6,
                        });
                        self.gains.extend_from_slice(&g[..channels]);
                    }
                }
                state.control += 512;
            }
            m.update(
                i,
                self.blocks.iter().copied(),
                &self.gains,
                self.plan.tracks[i].gain,
            )
            .expect("bounded validated dynamic curve");
        }
        Ok(())
    }
    #[allow(clippy::too_many_arguments)]
    pub fn point(
        &mut self,
        track: usize,
        input: &[f32],
        output: &mut [f32],
        live: &[f32],
        start: u64,
        frames: usize,
        user_gain: bool,
    ) -> Result<()> {
        self.dynamic.as_mut().ok_or_else(invalid)?.speaker(
            input,
            output,
            live,
            start,
            frames,
            Some(track),
            if user_gain { None } else { Some(1.) },
        )
    }
    pub fn process(
        &mut self,
        input: &[f32],
        out: &mut [f32],
        live: &[f32],
        start: u64,
        frames: usize,
        final_block: bool,
    ) -> Result<()> {
        let inputs = self.inputs();
        let channels = self.outputs();
        let samples = count(frames, channels)?;
        if frames > self.max_frames
            || start.checked_add(frames as u64).is_none()
            || input.len() < count(frames, inputs)?
            || !input.len().is_multiple_of(inputs)
            || out.len() < samples
            || !out.len().is_multiple_of(channels)
            || (!live.is_empty()
                && (live.len() < frames * inputs || !live.len().is_multiple_of(inputs)))
            || (!final_block && !frames.is_multiple_of(512))
        {
            return Err(invalid());
        }
        if frames == 0 {
            return Ok(());
        }
        // Validate every size lane before modifying any lane or caller output.
        for (i, p) in self.processors.iter().enumerate() {
            if !p.aligned_at(start) || p.required(frames, final_block)? != samples {
                return Err(invalid());
            }
            let ch = self.plan.tracks[i].input;
            if (0..frames).any(|f| !input[f * inputs + ch].is_finite()) {
                return Err(invalid());
            }
            if !self.in_matrix[i]
                && (self.weights[i] != 1. || self.targets[i] != 1.)
                && self.dynamic.is_none()
            {
                return Err(invalid());
            }
        }
        for i in 0..self.size_count() {
            let track = &self.plan.tracks[i];
            for f in 0..frames {
                self.input[f] = input[f * inputs + track.input];
            }
            self.processors[i].process_validated(
                &self.input[..frames],
                &mut self.output[..samples],
                final_block,
            );
            if self.in_matrix[i] {
                continue;
            }
            let point = self.weights[i] != 1. || self.targets[i] != 1.;
            if point {
                self.point_output[..samples].fill(0.);
                self.dynamic
                    .as_mut()
                    .unwrap()
                    .speaker(
                        input,
                        &mut self.point_output[..samples],
                        &[],
                        start,
                        frames,
                        Some(i),
                        Some(1.),
                    )
                    .expect("validated dry point mix");
            }
            for f in 0..frames {
                let t = ((f + 1) as f32 / 512.).min(1.);
                let weight = self.weights[i] + (self.targets[i] - self.weights[i]) * t;
                for c in 0..channels {
                    let at = f * channels + c;
                    let mut value = self.output[at];
                    if point {
                        value = if weight == 0. {
                            self.point_output[at]
                        } else {
                            value * weight + self.point_output[at] * (1. - weight)
                        };
                    }
                    value *= track.gain;
                    if !live.is_empty() {
                        value *= live[f * inputs + track.input];
                    }
                    out[at] += value;
                }
            }
            self.weights[i] = self.targets[i];
        }
        self.window = None;
        Ok(())
    }
    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            plan: Arc::clone(&self.plan),
            states: self.processors.iter().map(|p| p.state.clone()).collect(),
            points: self.points.clone(),
            curve_start: self.curve_start.clone(),
            window: self.window,
            max_frames: self.max_frames,
            interpolation: self.interpolation,
            weights: self.weights.clone(),
            targets: self.targets.clone(),
            in_matrix: self.in_matrix.clone(),
            live: self.dynamic.is_some(),
        }
    }
    fn compatible(&self, s: &Snapshot) -> bool {
        self.max_frames == s.max_frames
            && self.interpolation == s.interpolation
            && Arc::ptr_eq(&self.plan, &s.plan)
            && s.live == self.dynamic.is_some()
            && s.states.len() == self.size_count()
    }
    pub fn capture(&self, s: &mut Snapshot) -> Result<()> {
        if !self.compatible(s) {
            return Err(invalid());
        }
        for (i, p) in self.processors.iter().enumerate() {
            s.states[i].clone_from(&p.state);
        }
        s.points.clone_from_slice(&self.points);
        s.curve_start.clone_from_slice(&self.curve_start);
        s.window = self.window;
        s.weights.copy_from_slice(&self.weights);
        s.targets.copy_from_slice(&self.targets);
        s.in_matrix.copy_from_slice(&self.in_matrix);
        Ok(())
    }
    pub fn restore(&mut self, s: &Snapshot) -> Result<()> {
        if !self.compatible(s) {
            return Err(invalid());
        }
        for (i, p) in self.processors.iter_mut().enumerate() {
            p.state.clone_from(&s.states[i]);
        }
        self.points.clone_from_slice(&s.points);
        self.curve_start.clone_from_slice(&s.curve_start);
        self.window = s.window;
        self.weights.copy_from_slice(&s.weights);
        self.targets.copy_from_slice(&s.targets);
        self.in_matrix.copy_from_slice(&s.in_matrix);
        if let Some(m) = &mut self.dynamic {
            m.reset();
        }
        self.blocks.clear();
        self.gains.clear();
        if let Some((start, frames)) = s.window {
            self.points.clone_from_slice(&s.curve_start);
            self.prepare_points(start, frames)
                .expect("compatible saved point window");
        }
        Ok(())
    }
    pub fn snapshot_bytes(&self) -> Result<usize> {
        let per = size_of::<State>()
            + if self.dynamic.is_some() {
                2 * size_of::<Point>()
            } else {
                0
            }
            + size_of::<f32>() * 2
            + size_of::<bool>();
        count(self.size_count(), per)?
            .checked_add(size_of::<Snapshot>())
            .ok_or_else(invalid)
    }
}
