//! Recursive four-lane filter and buffered 512-frame object processor.
#![allow(clippy::excessive_precision, clippy::needless_range_loop)]
use super::{
    Event, Layout, Position, count, invalid,
    panner::{self, Gains, Mix},
};
use crate::{Error, Result};
use std::sync::Arc;
pub type FilteredFrame = [f32; 4];
#[derive(Clone)]
pub struct Decorrelator {
    delays: [[[f32; 4]; 346]; 4],
    indices: [usize; 4],
    input: [f32; 96],
    input_index: usize,
    pre: f32,
    post: f32,
    slow: f32,
    fast: f32,
    prefilter: f32,
    initial: bool,
    silent: u32,
}
impl Default for Decorrelator {
    fn default() -> Self {
        Self {
            delays: [[[0.; 4]; 346]; 4],
            indices: [0; 4],
            input: [0.; 96],
            input_index: 0,
            pre: 1.,
            post: 1.,
            slow: 0.,
            fast: 0.,
            prefilter: 0.,
            initial: true,
            silent: 0,
        }
    }
}
impl Decorrelator {
    fn reset_dsp(&mut self) {
        self.delays.fill([[0.; 4]; 346]);
        self.indices.fill(0);
        self.input.fill(0.);
        self.input_index = 0;
        self.pre = 1.;
        self.post = 1.;
        self.slow = 0.;
        self.fast = 0.;
        self.prefilter = 0.;
        self.initial = true;
    }
    pub fn reset(&mut self) {
        self.reset_dsp();
        self.silent = 0;
    }
    // Shared arithmetic only: the offline/file streaming and Scene streaming adapters own their silence clocks.
    fn sample(&mut self, x: f32, fade: f32) -> FilteredFrame {
        const DELAYS: [usize; 4] = [152, 200, 263, 346];
        const COEFF: [[f32; 4]; 4] = [
            [-0.4, -0.4, -0.4, -0.4],
            [-0.4, 0.4, -0.4, 0.4],
            [-0.4, -0.4, 0.4, 0.4],
            [-0.4, 0.4, 0.4, -0.4],
        ];
        let pref = 0.6752336621284485 * (x + self.prefilter);
        self.prefilter = pref - x;
        let level = pref.abs() + 1e-8;
        self.fast = 0.00415802001953125 * level + 0.9958419799804688 * self.fast;
        self.slow = 0.00026035308837890625 * level + 0.9997396469116211 * self.slow;
        self.pre = (self.pre - 1.) * 0.9995834231376648 + 1.;
        self.post = (self.post - 1.) * 0.9995834231376648 + 1.;
        let slow = if self.slow == 0. { 1e-8 } else { self.slow };
        let fast = if self.fast == 0. { 1e-8 } else { self.fast };
        // std::min retains its first argument when the comparison is unordered.
        let next = slow * 1.1 / fast;
        if next < self.pre {
            self.pre = next;
        }
        let next = fast * 1.1 / slow;
        if next < self.post {
            self.post = next;
        }
        let delayed = self.input[self.input_index];
        self.input[self.input_index] = x;
        self.input_index = (self.input_index + 1) % 96;
        let mut signal = [delayed * self.pre; 4];
        for stage in 0..4 {
            let memory = &mut self.delays[stage][self.indices[stage]];
            for mode in 0..4 {
                let next = COEFF[stage][mode] * memory[mode] + signal[mode];
                signal[mode] = memory[mode] - COEFF[stage][mode] * next;
                memory[mode] = next;
            }
            self.indices[stage] = (self.indices[stage] + 1) % DELAYS[stage];
        }
        signal.map(|v| (v * self.post) * fade)
    }
    pub fn process(&mut self, input: &[f32; 32], out: &mut [FilteredFrame; 32]) {
        let silent = input.iter().all(|x| x.abs() <= 0.00000011920928955078125);
        if !silent {
            self.silent = 0;
        } else if self.silent > 15 {
            out.fill([0.; 4]);
            return;
        }
        for f in 0..32 {
            let x = if self.initial {
                input[f] * (f as f32 / 32.)
            } else {
                input[f]
            };
            let fade = if silent && self.silent == 15 {
                (32 - f) as f32 / 32.
            } else {
                1.
            };
            out[f] = self.sample(x, fade);
        }
        self.initial = false;
        if silent {
            if self.silent == 15 {
                self.reset_dsp();
            }
            self.silent += 1;
        }
    }
}
/// Causal Scene variant: no lookahead or producer-block-dependent silence decisions.
#[derive(Default)]
pub(crate) struct LiveDecorrelator {
    filter: Decorrelator,
    startup: u32,
    silent: u32,
}
impl LiveDecorrelator {
    pub fn reset(&mut self) {
        self.filter.reset();
        self.startup = 0;
        self.silent = 0;
    }
    pub fn process(&mut self, input: f32) -> FilteredFrame {
        if input.abs() > 0.00000011920928955078125 {
            self.silent = 0;
        } else if self.silent == 512 {
            return [0.; 4];
        } else {
            self.silent += 1;
        }
        let x = if self.startup < 32 {
            input * (self.startup as f32 / 32.)
        } else {
            input
        };
        self.startup = (self.startup + 1).min(32);
        let fade = if self.silent > 480 {
            (513 - self.silent) as f32 / 32.
        } else {
            1.
        };
        let output = self.filter.sample(x, fade);
        if self.silent == 512 {
            self.filter.reset();
            self.startup = 0;
        }
        output
    }
}
pub struct Track {
    pub events: Box<[Event]>,
    pub layout: Layout,
}
impl Track {
    /// Events use the processor's internal coordinates, retaining their binary32 conversion.
    pub fn new(events: Vec<Event>, layout: Layout, sample_rate: u32) -> Result<Self> {
        if sample_rate != 48000 {
            return Err(Error::Unsupported("Triple Balance size requires 48 kHz"));
        }
        if events.first().is_none_or(|e| e.start != 0) {
            return Err(Error::Unsupported(
                "Triple Balance requires an event at frame zero",
            ));
        }
        for (i, e) in events.iter().enumerate() {
            if i > 0 && e.start < events[i - 1].start {
                return Err(Error::Unsupported(
                    "Triple Balance requires nondecreasing metadata timestamps",
                ));
            }
            if layout == Layout::Room222 {
                panner::room(e.position.adm(), e.size)?;
            } else {
                panner::quantize(e.position, e.size).map_err(|_| {
                    Error::Unsupported(
                        "Triple Balance size requires finite internal room coordinates",
                    )
                })?;
            }
        }
        Ok(Self {
            events: events.into_boxed_slice(),
            layout,
        })
    }

    /// The initial control block uses its last submitted target, like later blocks.
    pub(super) fn initial_event(&self) -> (usize, Event) {
        let next = self.events.partition_point(|e| e.start < 512);
        (next, self.events[next - 1])
    }
}
#[derive(Clone)]
pub struct State {
    next: usize,
    control: u64,
    pending: [f32; 512],
    pending_frames: usize,
    finished: bool,
    first: bool,
    position: Position,
    target: Position,
    size: f32,
    target_size: f32,
    previous: Mix,
    previous_point: Gains,
    cached: Mix,
    cached_point: Gains,
    cached_position: Position,
    cached_size: f32,
    previous_active: bool,
    older_active: bool,
    decorrelator: Decorrelator,
    scale: f32,
    source_size: f32,
}
impl Default for State {
    fn default() -> Self {
        Self {
            next: 1,
            control: 0,
            pending: [0.; 512],
            pending_frames: 0,
            finished: false,
            first: true,
            position: Position::default(),
            target: Position::default(),
            size: 0.,
            target_size: 0.,
            previous: Mix {
                direct: 1.,
                spread: [0.; 24],
            },
            previous_point: [0.; 24],
            cached: Mix::default(),
            cached_point: [0.; 24],
            cached_position: Position::default(),
            cached_size: -1.,
            previous_active: false,
            older_active: false,
            decorrelator: Decorrelator::default(),
            scale: 1.,
            source_size: 0.,
        }
    }
}
/// Private diagnostic query used to verify event, quantization and lifecycle decisions.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TbStatus {
    pub control: u64,
    pub next_event: usize,
    pub pending: usize,
    pub position: Position,
    pub size: f32,
    pub quantized: [i32; 4],
    pub flags: u32,
}
pub struct Processor {
    pub(crate) track: Arc<Track>,
    pub(crate) state: State,
    filtered: [FilteredFrame; 512],
}
pub struct Snapshot {
    track: Arc<Track>,
    state: State,
}
impl Processor {
    pub fn new(track: Arc<Track>) -> Self {
        let mut p = Self {
            track,
            state: State::default(),
            filtered: [[0.; 4]; 512],
        };
        p.reset();
        p
    }
    pub(crate) fn aligned_at(&self, start: u64) -> bool {
        self.state.control == start && self.state.pending_frames == 0
    }
    pub fn channels(&self) -> usize {
        self.track.layout.channels()
    }
    pub fn reset(&mut self) {
        let (next, event) = self.track.initial_event();
        let s = &mut self.state;
        s.next = next;
        s.control = 0;
        s.pending_frames = 0;
        s.pending.fill(0.);
        s.finished = false;
        s.first = true;
        s.previous = Mix {
            direct: 1.,
            spread: [0.; 24],
        };
        s.cached_size = -1.;
        s.previous_point.fill(0.);
        s.previous_active = false;
        s.older_active = false;
        s.decorrelator.reset();
        s.position = event.position;
        s.target = event.position;
        s.source_size = event.size;
        s.size = (s.source_size * s.scale).clamp(0., 1.);
        s.target_size = s.size;
    }
    pub fn set_scale(&mut self, scale: f32) -> Result<()> {
        if !scale.is_finite() || scale < 0. {
            return Err(invalid());
        }
        self.set_scale_validated(scale);
        Ok(())
    }
    pub(crate) fn set_scale_validated(&mut self, scale: f32) {
        let s = &mut self.state;
        s.scale = scale;
        s.target_size = (s.source_size * scale).clamp(0., 1.);
        if s.first {
            s.size = s.target_size;
        }
    }
    pub fn required(&self, input: usize, finish: bool) -> Result<usize> {
        if self.state.finished {
            return Err(invalid());
        }
        let total = self
            .state
            .pending_frames
            .checked_add(input)
            .ok_or_else(invalid)?;
        let controls = total / 512 + usize::from(finish && !total.is_multiple_of(512));
        self.state
            .control
            .checked_add(u64::try_from(count(controls, 512)?).map_err(|_| invalid())?)
            .ok_or_else(invalid)?;
        count(
            if finish { total } else { total / 512 * 512 },
            self.channels(),
        )
    }
    pub fn validate(&self, input: &[f32], output_len: usize, finish: bool) -> Result<usize> {
        let required = self.required(input.len(), finish)?;
        if output_len < required
            || !output_len.is_multiple_of(self.channels())
            || input.iter().any(|x| !x.is_finite())
        {
            return Err(invalid());
        }
        Ok(required)
    }
    pub fn process(&mut self, input: &[f32], output: &mut [f32], finish: bool) -> Result<usize> {
        let samples = self.validate(input, output.len(), finish)?;
        self.process_validated(input, &mut output[..samples], finish);
        Ok(samples / self.channels())
    }
    pub(crate) fn process_validated(
        &mut self,
        mut input: &[f32],
        output: &mut [f32],
        finish: bool,
    ) {
        let channels = self.channels();
        let mut written = 0;
        while !input.is_empty() {
            let take = input.len().min(512 - self.state.pending_frames);
            let begin = self.state.pending_frames;
            self.state.pending[begin..begin + take].copy_from_slice(&input[..take]);
            self.state.pending_frames += take;
            input = &input[take..];
            if self.state.pending_frames == 512 {
                self.control(&mut output[written..written + 512 * channels], 512);
                written += 512 * channels;
                self.state.pending_frames = 0;
            }
        }
        if finish {
            let n = self.state.pending_frames;
            if n > 0 {
                self.state.pending[n..].fill(0.);
                self.control(&mut output[written..written + n * channels], n);
            }
            self.state.finished = true;
            self.state.pending_frames = 0;
        }
    }
    fn control(&mut self, out: &mut [f32], frames: usize) {
        let layout = self.track.layout;
        let channels = layout.channels();
        let s = &mut self.state;
        while s.next < self.track.events.len() && self.track.events[s.next].start < s.control + 512
        {
            let e = self.track.events[s.next];
            s.next += 1;
            s.target = e.position;
            s.source_size = e.size;
            s.target_size = (s.source_size * s.scale).clamp(0., 1.);
        }
        if !s.first {
            s.position.advance(s.target);
            let alpha = 1. - (-512f32 / 960.).exp();
            s.size += alpha * (s.target_size - s.size);
            if s.target_size == 0. && s.size < 0.005 {
                s.size = 0.;
            }
        }
        let (mix, point) = if layout == Layout::Room222 {
            if s.cached_size != s.size || s.cached_position != s.position {
                let mut p = s.position.adm();
                s.cached = panner::room_mix(
                    panner::room(p, s.size).expect("validated room position"),
                    s.size,
                );
                if s.cached.direct == 0. {
                    p.z = 0.;
                }
                s.cached_point = panner::room(p, 0.).expect("validated room point");
                s.cached_position = s.position;
                s.cached_size = s.size;
            }
            (s.cached, s.cached_point)
        } else {
            let q = panner::quantize(s.position, s.size).expect("validated size parameters");
            let mix = panner::size_mix(
                panner::raw(q).expect("validated quantized parameters"),
                s.size,
            );
            let mut p = s.position.adm();
            if mix.direct == 0. {
                p.z = 0.;
            }
            (
                mix,
                panner::point(p, 1., layout).expect("validated point position"),
            )
        };
        if s.first {
            s.previous = mix;
            s.previous_point = point;
        }
        self.filtered.fill([0.; 4]);
        let active = s.size > 0.;
        if active || s.previous_active || s.older_active {
            for frame in (0..512).step_by(32) {
                s.decorrelator.process(
                    s.pending[frame..frame + 32].try_into().unwrap(),
                    (&mut self.filtered[frame..frame + 32]).try_into().unwrap(),
                );
            }
        } else {
            s.decorrelator.reset();
        }
        s.older_active = s.previous_active;
        s.previous_active = active;
        out.fill(0.);
        const MAP7: [usize; 11] = [0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11];
        const MAP9: [usize; 11] = [0, 1, 2, 4, 5, 6, 7, 10, 11, 14, 15];
        const FILTER: [i32; 11] = [0, 0, -1, 1, 1, 2, 2, 3, 3, 0, 0];
        const SIGN: [f32; 11] = [1., -1., 0., 1., -1., 1., -1., 1., -1., 1., -1.];
        for f in 0..frames {
            let now = (f + 1) as f32 / 512.;
            let before = 1. - now;
            let dry = s.pending[f] * (s.previous.direct * before + mix.direct * now);
            if layout == Layout::Room222 {
                for node in &panner::NODES {
                    let c = node.channel;
                    let mut wet = s.pending[f];
                    if node.filter >= 0 {
                        wet = 0.9219544529914856 * wet
                            + 0.3872983455657959
                                * node.sign
                                * self.filtered[f][node.filter as usize];
                    }
                    let gain = s.previous.spread[c] * before + mix.spread[c] * now;
                    out[f * channels + c] =
                        dry * (s.previous_point[c] * before + point[c] * now) + gain * wet;
                }
            } else {
                for c in 0..channels {
                    out[f * channels + c] = dry * (s.previous_point[c] * before + point[c] * now);
                }
                let map = if layout == Layout::Seven {
                    &MAP7
                } else {
                    &MAP9
                };
                for c in 0..11 {
                    let gain = s.previous.spread[c] * before + mix.spread[c] * now;
                    let mut sample = s.pending[f];
                    if FILTER[c] >= 0 {
                        sample = 0.9219544529914856 * sample
                            + 0.3872983455657959 * SIGN[c] * self.filtered[f][FILTER[c] as usize];
                    }
                    out[f * channels + map[c]] += gain * sample;
                }
            }
        }
        if !active {
            s.decorrelator.reset();
        }
        s.first = false;
        s.previous = mix;
        s.previous_point = point;
        s.control += 512;
    }
    pub fn status(&self) -> TbStatus {
        let s = &self.state;
        TbStatus {
            control: s.control,
            next_event: s.next,
            pending: s.pending_frames,
            position: s.position,
            size: s.size,
            quantized: if self.track.layout == Layout::Room222 {
                [-1; 4]
            } else {
                panner::quantize(s.position, s.size).expect("valid state")
            },
            flags: u32::from(s.first)
                | (u32::from(s.finished) << 1)
                | (u32::from(s.previous_active) << 2)
                | (u32::from(s.older_active) << 3)
                | (u32::from(s.previous.direct == 0.) << 4),
        }
    }
    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            track: Arc::clone(&self.track),
            state: self.state.clone(),
        }
    }
    pub fn capture(&self, out: &mut Snapshot) -> Result<()> {
        if !Arc::ptr_eq(&self.track, &out.track) {
            return Err(invalid());
        }
        out.state.clone_from(&self.state);
        Ok(())
    }
    pub fn restore(&mut self, snapshot: &Snapshot) -> Result<()> {
        if !Arc::ptr_eq(&self.track, &snapshot.track) {
            return Err(invalid());
        }
        self.state.clone_from(&snapshot.state);
        Ok(())
    }
    pub fn snapshot_bytes() -> usize {
        size_of::<Snapshot>()
    }
}
