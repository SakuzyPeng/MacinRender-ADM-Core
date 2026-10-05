//! Monitor conditioning. Each instance belongs to exactly one render/callback thread.
use crate::{Error, Result};

pub const LEVEL_CHANNELS: usize = 64;

fn invalid() -> Error {
    Error::InvalidArgument("Invalid monitor DSP arguments")
}
fn channels_valid(channels: usize) -> Result<()> {
    if channels == 0 || channels > isize::MAX as usize / size_of::<f32>() {
        return Err(invalid());
    }
    Ok(())
}
fn samples(channels: usize, frames: usize, length: usize) -> Result<usize> {
    let needed = channels.checked_mul(frames).ok_or_else(invalid)?;
    if length > isize::MAX as usize / size_of::<f32>()
        || !length.is_multiple_of(channels)
        || length < needed
    {
        return Err(invalid());
    }
    Ok(needed)
}

/// Worker-owned backend blend. Stream lifetime and early-EOS decisions stay with the caller.
#[derive(Clone, Debug, PartialEq)]
pub struct Crossfade {
    channels: usize,
    total: u64,
    position: u64,
}
impl Crossfade {
    pub fn new(channels: usize, total: u64) -> Result<Self> {
        channels_valid(channels)?;
        if total == 0 {
            return Err(invalid());
        }
        Ok(Self {
            channels,
            total,
            position: 0,
        })
    }
    pub fn reset(&mut self) {
        self.position = 0;
    }
    pub fn process(&mut self, old: &mut [f32], incoming: &[f32], frames: usize) -> Result<bool> {
        samples(self.channels, frames, old.len())?;
        samples(self.channels, frames, incoming.len())?;
        let end = self
            .position
            .checked_add(u64::try_from(frames).map_err(|_| invalid())?)
            .ok_or_else(invalid)?;
        for frame in 0..frames {
            let p = (self.position + frame as u64) as f64;
            let t = (p / self.total as f64).min(1.0) as f32;
            for channel in 0..self.channels {
                let i = frame * self.channels + channel;
                old[i] = (old[i] * (1.0 - t)) + (incoming[i] * t);
            }
        }
        self.position = end;
        Ok(end >= self.total)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Callback {
    pub frames: usize,
    pub produced_frames: usize,
    pub active: bool,
    pub generation: u64,
}

/// Callback-owned seek history; levels are returned to the caller for atomic publication.
#[derive(Clone, Debug, PartialEq)]
pub struct Output {
    channels: usize,
    realtime: bool,
    total: usize,
    remaining: usize,
    generation: u64,
    last: Vec<f32>,
    anchor: Vec<f32>,
}
impl Output {
    pub fn new(channels: usize, rate: u32, realtime: bool) -> Result<Self> {
        channels_valid(channels)?;
        if rate == 0 {
            return Err(invalid());
        }
        let total = (usize::try_from(rate)
            .map_err(|_| invalid())?
            .checked_mul(10)
            .ok_or_else(invalid)?
            / 1000)
            .max(1);
        Ok(Self {
            channels,
            realtime,
            total,
            remaining: 0,
            generation: 0,
            last: vec![0.0; channels],
            anchor: vec![0.0; channels],
        })
    }
    pub fn reset(&mut self) {
        self.remaining = 0;
        self.generation = 0;
        self.last.fill(0.0);
        self.anchor.fill(0.0);
    }
    pub fn process(
        &mut self,
        pcm: &mut [f32],
        call: Callback,
        peak: &mut [f32],
        rms: &mut [f32],
    ) -> Result<()> {
        samples(self.channels, call.frames, pcm.len())?;
        let meter_channels = self.channels.min(LEVEL_CHANNELS);
        if call.produced_frames > call.frames
            || peak.len() < meter_channels
            || rms.len() < meter_channels
        {
            return Err(invalid());
        }
        self.transition(pcm, call);
        for channel in 0..meter_channels {
            let mut maximum = 0.0_f32;
            let mut sum = 0.0_f64;
            for frame in 0..call.frames {
                let value = pcm[frame * self.channels + channel];
                // Match std::max(accumulator, abs(value)): a NaN input leaves Peak unchanged.
                if maximum < value.abs() {
                    maximum = value.abs();
                }
                sum += f64::from(value) * f64::from(value);
            }
            peak[channel] = maximum;
            rms[channel] = if call.frames == 0 {
                0.0
            } else {
                (sum / call.frames as f64).sqrt() as f32
            };
        }
        Ok(())
    }
    fn transition(&mut self, pcm: &mut [f32], call: Callback) {
        if call.frames == 0 {
            return;
        }
        if !call.active {
            self.last.fill(0.0);
            self.remaining = 0;
            return;
        }
        let real = call.produced_frames;
        if real > 0 && call.generation != self.generation {
            self.generation = call.generation;
            self.remaining = self.total;
            if self.realtime {
                self.anchor.copy_from_slice(&self.last);
            } else {
                self.anchor.fill(0.0);
            }
        }
        for frame in 0..real.min(self.remaining) {
            let elapsed = self.total - self.remaining;
            let mix = if self.total <= 1 {
                1.0
            } else {
                elapsed as f32 / (self.total - 1) as f32
            };
            for channel in 0..self.channels {
                let i = frame * self.channels + channel;
                pcm[i] = (self.anchor[channel] * (1.0 - mix)) + (pcm[i] * mix);
            }
            self.remaining -= 1;
        }
        let emitted = if self.realtime { call.frames } else { real };
        if emitted > 0 {
            let start = (emitted - 1) * self.channels;
            self.last
                .copy_from_slice(&pcm[start..start + self.channels]);
        }
        if self.realtime && real < call.frames {
            self.remaining = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn counter_overflow_is_atomic() {
        let mut fade = Crossfade::new(1, 2048).unwrap();
        fade.position = u64::MAX;
        let mut pcm = [0.25];
        assert!(fade.process(&mut pcm, &[0.75], 1).is_err());
        assert_eq!(pcm, [0.25]);
        assert_eq!(fade.position, u64::MAX);
    }
}
