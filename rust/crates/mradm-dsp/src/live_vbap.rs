//! Live Scene speaker mixing. Metadata semantics and PCM ownership stay with C++.
use crate::{Error, Result};

pub const PAN: u32 = 1;
pub const LEVEL: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Command {
    pub element: u32,
    pub offset: u32,
    pub duration: u32,
    pub fields: u32,
    pub coefficient_offset: u64,
    pub level: f32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Status {
    pub current_level: f32,
    pub target_level: f32,
    pub level_step: f32,
    pub level_remaining: u32,
    pub pan_remaining: u32,
}
impl Default for Status {
    fn default() -> Self {
        Self {
            current_level: 1.,
            target_level: 1.,
            level_step: 0.,
            level_remaining: 0,
            pan_remaining: 0,
        }
    }
}
fn invalid() -> Error {
    Error::InvalidArgument("Invalid Live VBAP numerical arguments")
}

#[derive(Clone, Debug, PartialEq)]
pub struct Mixer {
    channels: usize,
    current: Vec<f32>,
    target: Vec<f32>,
    steps: Vec<f32>,
    states: Vec<Status>,
}
impl Mixer {
    pub fn new(elements: u32, channels: u32) -> Result<Self> {
        let channels = channels as usize;
        let elements = elements as usize;
        let count = elements.checked_mul(channels).ok_or_else(invalid)?;
        if channels == 0
            || count > isize::MAX as usize / size_of::<f32>()
            || elements > isize::MAX as usize / size_of::<Status>()
        {
            return Err(invalid());
        }
        Ok(Self {
            channels,
            current: vec![0.; count],
            target: vec![0.; count],
            steps: vec![0.; count],
            states: vec![Status::default(); elements],
        })
    }
    pub fn reset(&mut self) {
        self.current.fill(0.);
        self.target.fill(0.);
        self.steps.fill(0.);
        self.states.fill(Status::default());
    }
    /// Private diagnostic copy used to compare exact ramp states, never called per sample in production.
    pub fn snapshot(&self, element: u32, gains: &mut [f32]) -> Result<Status> {
        let element = element as usize;
        if element >= self.states.len()
            || gains.len() != self.channels.checked_mul(3).ok_or_else(invalid)?
        {
            return Err(invalid());
        }
        let begin = element * self.channels;
        for (out, values) in
            gains
                .chunks_exact_mut(self.channels)
                .zip([&self.current, &self.target, &self.steps])
        {
            out.copy_from_slice(&values[begin..begin + self.channels]);
        }
        Ok(self.states[element])
    }
    fn validate_command(
        &self,
        c: &Command,
        coefficients: &[f32],
        frames: u32,
        initial: bool,
    ) -> Result<()> {
        if c.element as usize >= self.states.len()
            || c.fields == 0
            || c.fields & !(PAN | LEVEL) != 0
            || c.reserved != 0
            || (c.fields & LEVEL != 0 && !c.level.is_finite())
            || (initial && (c.fields != PAN | LEVEL || c.offset != 0 || c.duration != 0))
            || (!initial && c.offset >= frames)
        {
            return Err(invalid());
        }
        if c.fields & PAN != 0 {
            let offset = usize::try_from(c.coefficient_offset).map_err(|_| invalid())?;
            if offset.checked_add(self.channels).ok_or_else(invalid)? > coefficients.len() {
                return Err(invalid());
            }
        }
        Ok(())
    }
    /// Cloneable borrowed views avoid a per-call allocation or a copy of the planar PCM.
    pub fn process<'a, I>(
        &mut self,
        frames: u32,
        planes: I,
        initial: &[Command],
        events: &[Command],
        coefficients: &[f32],
        output: &mut [f32],
    ) -> Result<()>
    where
        I: Clone + ExactSizeIterator<Item = Option<&'a [f32]>>,
    {
        let frames_usize = frames as usize;
        let samples = frames_usize
            .checked_mul(self.channels)
            .ok_or_else(invalid)?;
        if output.len() < samples
            || !output.len().is_multiple_of(self.channels)
            || planes.len() != self.states.len()
            || coefficients.iter().any(|v| !v.is_finite())
        {
            return Err(invalid());
        }
        for plane in planes.clone().flatten() {
            if plane.len() < frames_usize {
                return Err(invalid());
            }
        }
        for c in initial {
            self.validate_command(c, coefficients, frames, true)?;
        }
        let mut previous = 0;
        for c in events {
            self.validate_command(c, coefficients, frames, false)?;
            if c.offset < previous {
                return Err(invalid());
            }
            previous = c.offset;
        }
        // No fallible work after this point. Every source, command and output has been checked.
        output[..samples].fill(0.);
        for c in initial {
            self.apply(c, coefficients);
        }
        let mut start = 0;
        for c in events {
            self.mix(planes.clone(), start, c.offset as usize, output);
            self.apply(c, coefficients);
            start = c.offset as usize;
        }
        self.mix(planes, start, frames_usize, output);
        Ok(())
    }
    fn apply(&mut self, c: &Command, coefficients: &[f32]) {
        let element = c.element as usize;
        let state = &mut self.states[element];
        if c.fields & LEVEL != 0 {
            state.target_level = c.level;
            state.level_remaining = c.duration;
            state.level_step = if c.duration == 0 {
                0.
            } else {
                (c.level - state.current_level) / c.duration as f32
            };
            if c.duration == 0 {
                state.current_level = c.level;
            }
        }
        if c.fields & PAN != 0 {
            let offset = c.coefficient_offset as usize;
            let begin = element * self.channels;
            state.pan_remaining = c.duration;
            for channel in 0..self.channels {
                let index = begin + channel;
                self.target[index] = coefficients[offset + channel];
                if c.duration == 0 {
                    self.current[index] = self.target[index];
                    self.steps[index] = 0.;
                } else {
                    self.steps[index] =
                        (self.target[index] - self.current[index]) / c.duration as f32;
                }
            }
        }
    }
    fn mix<'a>(
        &mut self,
        planes: impl Iterator<Item = Option<&'a [f32]>>,
        start: usize,
        end: usize,
        output: &mut [f32],
    ) {
        if start == end {
            return; // Same-offset commands do not require a pass over every element.
        }
        // Reordering independent element loops leaves each output sample's source accumulation order intact.
        for (element, plane) in planes.enumerate() {
            let begin = element * self.channels;
            let state = &mut self.states[element];
            for frame in start..end {
                if let Some(input) = plane {
                    for channel in 0..self.channels {
                        output[frame * self.channels + channel] +=
                            (input[frame] * self.current[begin + channel]) * state.current_level;
                    }
                }
                if state.level_remaining != 0 {
                    state.current_level += state.level_step;
                    state.level_remaining -= 1;
                    if state.level_remaining == 0 {
                        state.current_level = state.target_level;
                    }
                }
                if state.pan_remaining != 0 {
                    for channel in 0..self.channels {
                        self.current[begin + channel] += self.steps[begin + channel];
                    }
                    state.pan_remaining -= 1;
                    if state.pan_remaining == 0 {
                        self.current[begin..begin + self.channels]
                            .copy_from_slice(&self.target[begin..begin + self.channels]);
                    }
                }
            }
        }
    }
}
