//! Sample-domain live gains. Control publication and ADM addressing remain in C++.
use crate::{Error, Result};

#[derive(Clone)]
struct Ramp {
    frames: usize,
    remaining: usize,
    current: f32,
    target: f32,
    step: f32,
    started: bool,
}

impl Ramp {
    fn new(frames: usize) -> Self {
        Self {
            frames,
            remaining: 0,
            current: 1.,
            target: 1.,
            step: 0.,
            started: false,
        }
    }
    fn set_target(&mut self, target: f32) {
        if target == self.target {
            return;
        }
        self.target = target;
        if !self.started {
            return;
        }
        if self.frames <= 1 || self.current == self.target {
            self.current = self.target;
            self.remaining = 0;
            self.step = 0.;
            return;
        }
        self.remaining = self.frames;
        self.step = (self.target - self.current) / (self.frames - 1) as f32;
    }
    fn next(&mut self) -> f32 {
        if !self.started {
            self.started = true;
            self.current = self.target;
            self.remaining = 0;
            return self.current;
        }
        let value = self.current;
        if self.remaining > 1 {
            self.current += self.step;
            self.remaining -= 1;
        } else if self.remaining == 1 {
            self.current = self.target;
            self.remaining = 0;
            self.step = 0.;
        }
        value
    }
}

/// One independent ramp per channel. A single-channel bank also generates mono envelopes.
#[derive(Clone)]
pub struct GainBank {
    ramps: Vec<Ramp>,
}

impl GainBank {
    pub fn new(channels: usize, sample_rate: u32, ramp_ms: u32) -> Result<Self> {
        if channels == 0 || channels > isize::MAX as usize / size_of::<Ramp>() || sample_rate == 0 {
            return Err(Error::InvalidArgument(
                "Invalid gain channel count or sample rate",
            ));
        }
        let frames = usize::try_from(u64::from(sample_rate) * u64::from(ramp_ms) / 1000)
            .map_err(|_| Error::InvalidArgument("Gain duration overflow"))?
            .max(1);
        Ok(Self {
            ramps: vec![Ramp::new(frames); channels],
        })
    }
    pub fn channels(&self) -> usize {
        self.ramps.len()
    }
    /// Restore the unstarted, unity state. Callers explicitly reapply their desired targets.
    pub fn reset(&mut self) {
        for ramp in &mut self.ramps {
            *ramp = Ramp::new(ramp.frames);
        }
    }
    /// Missing targets are unity; excess entries are ignored, as in the original smoother.
    pub fn set_targets(&mut self, targets: &[f32]) -> Result<()> {
        if targets.iter().take(self.channels()).any(|v| !v.is_finite()) {
            return Err(Error::InvalidArgument("Nonfinite live gain target"));
        }
        for (channel, ramp) in self.ramps.iter_mut().enumerate() {
            ramp.set_target(targets.get(channel).copied().unwrap_or(1.));
        }
        Ok(())
    }
    pub fn fill(&mut self, output: &mut [f32]) -> Result<()> {
        self.process(output, false)
    }
    pub fn apply(&mut self, pcm: &mut [f32]) -> Result<()> {
        self.process(pcm, true)
    }
    fn process(&mut self, output: &mut [f32], multiply: bool) -> Result<()> {
        if !output.len().is_multiple_of(self.channels()) {
            return Err(Error::InvalidArgument("Incomplete live gain frame"));
        }
        for frame in output.chunks_exact_mut(self.channels()) {
            for (sample, ramp) in frame.iter_mut().zip(&mut self.ramps) {
                let gain = ramp.next();
                if multiply {
                    *sample *= gain;
                } else {
                    *sample = gain;
                }
            }
        }
        Ok(())
    }
}
