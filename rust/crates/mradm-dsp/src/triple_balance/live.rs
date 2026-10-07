//! Incremental canonical Scene rendering. Controls use the input sample clock,
//! independent of the file renderer's 512-sample metadata grid.
#![allow(clippy::needless_range_loop, clippy::excessive_precision)]
use super::{Layout, Position, count, invalid, panner, processor::LiveDecorrelator};
use crate::{Error, Result};

pub const POSITION: u32 = 1;
pub const SIZE: u32 = 2;
pub const LEVEL: u32 = 4;
const ALL: u32 = POSITION | SIZE | LEVEL;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TbLiveElement {
    pub kind: u32, // 0 = object, 1 = fixed bed route
    pub reserved: u32,
    pub gains: [f32; 24],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TbLiveCommand {
    pub element: u32,
    pub offset: u32,
    pub duration: u32,
    pub fields: u32,
    pub position: Position,
    pub size: f32,
    pub level: f32,
}

#[derive(Clone, Copy)]
struct Ramp<const N: usize> {
    current: [f32; N],
    target: [f32; N],
    step: [f32; N],
    remaining: u32,
}
impl<const N: usize> Ramp<N> {
    fn new(value: [f32; N]) -> Self {
        Self {
            current: value,
            target: value,
            step: [0.; N],
            remaining: 0,
        }
    }
    fn set(&mut self, target: [f32; N], duration: u32) {
        self.target = target;
        self.remaining = duration;
        for i in 0..N {
            self.step[i] = if duration == 0 {
                0.
            } else {
                (target[i] - self.current[i]) / duration as f32
            };
        }
        if duration == 0 {
            self.current = target;
        }
    }
    fn advance(&mut self) {
        if self.remaining == 0 {
            return;
        }
        self.remaining -= 1;
        for i in 0..N {
            self.current[i] = if self.remaining == 0 {
                self.target[i]
            } else {
                (self.current[i] + self.step[i]).clamp(
                    self.current[i].min(self.target[i]),
                    self.current[i].max(self.target[i]),
                )
            };
        }
    }
}

struct Lane {
    descriptor: TbLiveElement,
    position: Ramp<3>,
    size: Ramp<1>,
    level: Ramp<1>,
    filter: Option<Box<LiveDecorrelator>>,
    cached: Option<(Position, f32)>,
    point: panner::Gains,
    mix: panner::Mix,
    size_active: bool,
}
impl Lane {
    fn new(descriptor: TbLiveElement) -> Self {
        Self {
            descriptor,
            position: Ramp::new([0., 1., 0.]),
            size: Ramp::new([0.]),
            level: Ramp::new([1.]),
            filter: (descriptor.kind == 0).then(|| Box::new(LiveDecorrelator::default())),
            cached: None,
            point: [0.; 24],
            mix: panner::Mix {
                direct: 1.,
                spread: [0.; 24],
            },
            size_active: false,
        }
    }
    fn reset(&mut self) {
        self.position = Ramp::new([0., 1., 0.]);
        self.size = Ramp::new([0.]);
        self.level = Ramp::new([1.]);
        self.cached = None;
        self.size_active = false;
        if let Some(f) = &mut self.filter {
            f.reset();
        }
    }
    fn spatial(&mut self, layout: Layout) {
        let [x, y, z] = self.position.current;
        let position = Position { x, y, z };
        let size = self.size.current[0];
        if self.cached == Some((position, size)) {
            return;
        }
        self.cached = Some((position, size));
        if size == 0. {
            self.mix = panner::Mix {
                direct: 1.,
                spread: [0.; 24],
            };
            self.point = panner::live_point(position, 1., layout).expect("validated live point");
        } else {
            self.mix = if layout == Layout::Room222 {
                panner::live_room_mix(
                    panner::live_room(position, size).expect("validated live room"),
                    size,
                )
            } else {
                let q = panner::quantize(position.internal(), size).expect("validated live size");
                panner::live_size_mix(
                    panner::live_raw(q).expect("validated live quantization"),
                    size,
                )
            };
            // Match the established size mix's dry point conversion and height rule.
            let mut point = position.internal().adm();
            if self.mix.direct == 0. {
                point.z = 0.;
            }
            self.point = panner::live_point(point, 1., layout).expect("validated live dry point");
        }
    }
    fn sample(&mut self, layout: Layout, input: f32, output: &mut [f32]) {
        if self.descriptor.kind == 1 {
            for (out, gain) in output.iter_mut().zip(self.descriptor.gains) {
                *out += (input * gain) * self.level.current[0];
            }
        } else {
            self.spatial(layout);
            let active = self.size.current[0] > 0.;
            let filter = self.filter.as_mut().unwrap();
            if !active && self.size_active {
                filter.reset();
            }
            let filtered = if active {
                filter.process(input)
            } else {
                [0.; 4]
            };
            self.size_active = active;
            let dry = input * self.mix.direct;
            let mut values = self.point.map(|gain| dry * gain);
            if layout == Layout::Room222 {
                for node in &panner::NODES {
                    let mut wet = input;
                    if node.filter >= 0 {
                        wet = 0.9219544529914856 * wet
                            + 0.3872983455657959 * node.sign * filtered[node.filter as usize];
                    }
                    values[node.channel] += self.mix.spread[node.channel] * wet;
                }
            } else {
                const MAP7: [usize; 11] = [0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11];
                const MAP9: [usize; 11] = [0, 1, 2, 4, 5, 6, 7, 10, 11, 14, 15];
                const FILTER: [i32; 11] = [0, 0, -1, 1, 1, 2, 2, 3, 3, 0, 0];
                const SIGN: [f32; 11] = [1., -1., 0., 1., -1., 1., -1., 1., -1., 1., -1.];
                let map = if layout == Layout::Seven {
                    &MAP7
                } else {
                    &MAP9
                };
                for c in 0..11 {
                    let mut wet = input;
                    if FILTER[c] >= 0 {
                        wet = 0.9219544529914856 * wet
                            + 0.3872983455657959 * SIGN[c] * filtered[FILTER[c] as usize];
                    }
                    values[map[c]] += self.mix.spread[c] * wet;
                }
            }
            for (out, value) in output.iter_mut().zip(values) {
                *out += value * self.level.current[0];
            }
        }
        self.position.advance();
        self.size.advance();
        self.level.advance();
    }
}

pub struct Mixer {
    layout: Layout,
    rate: u32,
    lanes: Vec<Lane>,
}
impl Mixer {
    pub fn new(layout: Layout, rate: u32, elements: &[TbLiveElement]) -> Result<Self> {
        if !(8000..=192000).contains(&rate) {
            return Err(invalid());
        }
        if rate != 48000 && (layout == Layout::Room222 || elements.iter().any(|e| e.kind == 1)) {
            return Err(Error::Unsupported(
                "Triple Balance bed and 22.2 require 48 kHz",
            ));
        }
        count(elements.len(), size_of::<Lane>())?;
        for e in elements {
            if e.kind > 1 || e.reserved != 0 || e.gains.iter().any(|g| !g.is_finite()) {
                return Err(invalid());
            }
        }
        panner::prepare_live();
        Ok(Self {
            layout,
            rate,
            lanes: elements.iter().copied().map(Lane::new).collect(),
        })
    }
    pub fn reset(&mut self) {
        for lane in &mut self.lanes {
            lane.reset();
        }
    }
    fn validate_command(&self, c: &TbLiveCommand, frames: u32, initial: bool) -> Result<()> {
        if c.element as usize >= self.lanes.len()
            || c.fields == 0
            || c.fields & !ALL != 0
            || (initial && (c.fields != ALL || c.offset != 0 || c.duration != 0))
            || (!initial && c.offset >= frames)
        {
            return Err(invalid());
        }
        if c.fields & POSITION != 0 {
            panner::live_point(c.position, 1., self.layout)?;
        }
        if c.fields & SIZE != 0 {
            if !c.size.is_finite() || !(0. ..=1.).contains(&c.size) {
                return Err(invalid());
            }
            if c.size != 0.
                && (self.rate != 48000 || self.lanes[c.element as usize].descriptor.kind != 0)
            {
                return Err(Error::Unsupported(
                    "Triple Balance size requires 48 kHz Objects",
                ));
            }
        }
        if c.fields & LEVEL != 0 && (!c.level.is_finite() || c.level < 0.) {
            return Err(invalid());
        }
        Ok(())
    }
    fn apply(&mut self, c: &TbLiveCommand) {
        let lane = &mut self.lanes[c.element as usize];
        if c.fields & POSITION != 0 {
            lane.position
                .set([c.position.x, c.position.y, c.position.z], c.duration);
        }
        if c.fields & SIZE != 0 {
            lane.size.set([c.size], c.duration);
        }
        if c.fields & LEVEL != 0 {
            lane.level.set([c.level], c.duration);
        }
    }
    pub fn process<'a, I>(
        &mut self,
        frames: u32,
        planes: I,
        initial: &[TbLiveCommand],
        events: &[TbLiveCommand],
        output: &mut [f32],
    ) -> Result<()>
    where
        I: Clone + ExactSizeIterator<Item = Option<&'a [f32]>>,
    {
        let channels = self.layout.channels();
        let samples = count(frames as usize, channels)?;
        if output.len() < samples
            || !output.len().is_multiple_of(channels)
            || planes.len() != self.lanes.len()
        {
            return Err(invalid());
        }
        for plane in planes.clone().flatten() {
            if plane.len() < frames as usize
                || plane[..frames as usize].iter().any(|x| !x.is_finite())
            {
                return Err(invalid());
            }
        }
        for command in initial {
            self.validate_command(command, frames, true)?;
        }
        let mut previous = 0;
        for c in events {
            self.validate_command(c, frames, false)?;
            if c.offset < previous {
                return Err(invalid());
            }
            previous = c.offset;
        }
        // All fallible work precedes changes to DSP history or caller output.
        if frames == 0 {
            return Ok(());
        }
        output[..samples].fill(0.);
        for c in initial {
            self.apply(c);
        }
        let mut start = 0;
        for c in events {
            self.mix(planes.clone(), start, c.offset as usize, output);
            self.apply(c);
            start = c.offset as usize;
        }
        self.mix(planes, start, frames as usize, output);
        Ok(())
    }
    fn mix<'a>(
        &mut self,
        planes: impl Iterator<Item = Option<&'a [f32]>>,
        start: usize,
        end: usize,
        output: &mut [f32],
    ) {
        let channels = self.layout.channels();
        for (lane, plane) in self.lanes.iter_mut().zip(planes) {
            for f in start..end {
                lane.sample(
                    self.layout,
                    plane.map_or(0., |p| p[f]),
                    &mut output[f * channels..(f + 1) * channels],
                );
            }
        }
    }
}
