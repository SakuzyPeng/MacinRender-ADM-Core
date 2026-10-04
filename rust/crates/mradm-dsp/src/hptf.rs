//! Headphone compensation: quantized biquads, double state and linear swaps.
//! Control publication and applied-state mailboxes belong to the C++ adapter.
mod design;
use crate::{Error, Result};
pub use design::design;

pub const MAX_BANDS: usize = 32;
pub const BLEND_FRAMES: usize = 2048;
pub const CHUNK_FRAMES: usize = 1024;

#[derive(Clone, Copy, Debug)]
pub enum BandKind {
    Peaking,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    BandPass,
    Notch,
}
impl TryFrom<u32> for BandKind {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Peaking),
            1 => Ok(Self::LowShelf),
            2 => Ok(Self::HighShelf),
            3 => Ok(Self::LowPass),
            4 => Ok(Self::HighPass),
            5 => Ok(Self::BandPass),
            6 => Ok(Self::Notch),
            _ => Err(Error::InvalidArgument("HpTF:未知的滤波器类型")),
        }
    }
}
#[derive(Clone, Copy)]
pub struct Band {
    pub kind: BandKind,
    pub enabled: bool,
    pub frequency: f64,
    pub gain_db: f64,
    pub q: f64,
}
#[derive(Clone, Copy)]
pub enum PreampMode {
    WarnOnly,
    AutoTrim,
}
impl TryFrom<u32> for PreampMode {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::WarnOnly),
            1 => Ok(Self::AutoTrim),
            _ => Err(Error::InvalidArgument("HpTF:未知的前级策略")),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Biquad {
    pub b0: f32,
    pub b1: f32,
    pub b2: f32,
    pub a1: f32,
    pub a2: f32,
}
impl Default for Biquad {
    fn default() -> Self {
        Self {
            b0: 1.0,
            b1: 0.0,
            b2: 0.0,
            a1: 0.0,
            a2: 0.0,
        }
    }
}
impl Biquad {
    fn validate(&self) -> Result<()> {
        let (a1, a2) = (self.a1 as f64, self.a2 as f64);
        if [self.b0, self.b1, self.b2, self.a1, self.a2]
            .iter()
            .any(|x| !x.is_finite())
            || 1.0 + a1 + a2 <= 0.0
            || 1.0 - a1 + a2 <= 0.0
            || 1.0 - a2 <= 0.0
        {
            return Err(Error::InvalidArgument(
                "HpTF:滤波器系数无效或在当前采样率下不稳定",
            ));
        }
        Ok(())
    }
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Coefficients {
    pub sample_rate: u32,
    pub band_count: usize,
    pub preamp_gain: f32,
    pub sections: [Biquad; MAX_BANDS],
    pub max_response_db: f32,
    pub auto_trim_db: f32,
    pub preamp_db: f32,
}
impl Default for Coefficients {
    fn default() -> Self {
        Self {
            sample_rate: 0,
            band_count: 0,
            preamp_gain: 1.0,
            sections: [Biquad::default(); MAX_BANDS],
            max_response_db: 0.0,
            auto_trim_db: 0.0,
            preamp_db: 0.0,
        }
    }
}
impl Coefficients {
    pub fn is_bypass(&self) -> bool {
        self.band_count == 0 && self.preamp_gain == 1.0
    }
    pub fn validate(&self) -> Result<()> {
        if self.band_count > MAX_BANDS
            || self.preamp_gain < 0.0
            || [
                self.preamp_gain,
                self.max_response_db,
                self.auto_trim_db,
                self.preamp_db,
            ]
            .iter()
            .any(|x| !x.is_finite())
        {
            return Err(Error::InvalidArgument("HpTF:无效的级联系数"));
        }
        for section in &self.sections[..self.band_count] {
            section.validate()?;
        }
        Ok(())
    }
    pub fn magnitude_db(&self, frequency: f64) -> Result<f64> {
        self.validate()?;
        if !frequency.is_finite() || !(std::f64::consts::TAU * frequency).is_finite() {
            return Err(Error::InvalidArgument("HpTF: invalid response frequency"));
        }
        Ok(design::cascade_response(self, frequency))
    }
}

#[derive(Clone)]
pub struct Cascade {
    coefficients: Coefficients,
    channels: usize,
    state: Vec<f64>,
}
impl Cascade {
    pub fn new(channels: usize) -> Result<Self> {
        let count = channels
            .checked_mul(MAX_BANDS * 2)
            .filter(|&n| n <= isize::MAX as usize / size_of::<f64>())
            .ok_or(Error::InvalidArgument("HpTF: channel count overflow"))?;
        Ok(Self {
            coefficients: Coefficients::default(),
            channels,
            state: vec![0.0; count],
        })
    }
    pub fn channels(&self) -> usize {
        self.channels
    }
    pub fn coefficients(&self) -> Coefficients {
        self.coefficients
    }
    pub fn set_coefficients(&mut self, coefficients: Coefficients) -> Result<()> {
        coefficients.validate()?;
        self.coefficients = coefficients;
        Ok(())
    }
    pub fn reset(&mut self) {
        self.state.fill(0.0);
    }
    fn validate_buffer(&self, length: usize) -> Result<()> {
        if (self.channels == 0 && length != 0)
            || (self.channels != 0 && !length.is_multiple_of(self.channels))
        {
            return Err(Error::InvalidArgument(
                "HpTF: incomplete interleaved frames",
            ));
        }
        Ok(())
    }
    pub fn process(&mut self, samples: &mut [f32]) -> Result<()> {
        self.validate_buffer(samples.len())?;
        self.process_validated(samples);
        Ok(())
    }
    fn process_validated(&mut self, samples: &mut [f32]) {
        if samples.is_empty() || self.channels == 0 || self.coefficients.is_bypass() {
            return;
        }
        let frames = samples.len() / self.channels;
        let preamp = self.coefficients.preamp_gain as f64;
        for channel in 0..self.channels {
            let state = &mut self.state[channel * MAX_BANDS * 2..(channel + 1) * MAX_BANDS * 2];
            for frame in 0..frames {
                let index = frame * self.channels + channel;
                let mut x = samples[index] as f64 * preamp;
                for (s, z) in self.coefficients.sections[..self.coefficients.band_count]
                    .iter()
                    .zip(state.as_chunks_mut::<2>().0)
                {
                    let y = (s.b0 as f64 * x) + z[0];
                    z[0] = ((s.b1 as f64 * x) - (s.a1 as f64 * y)) + z[1];
                    z[1] = (s.b2 as f64 * x) - (s.a2 as f64 * y);
                    x = y;
                }
                samples[index] = x as f32;
            }
        }
        // Preserve the original block-end recovery/denormal policy. Nonfinite
        // input may affect this block's output, but cannot poison future blocks.
        if self.state.iter().any(|x| !x.is_finite()) || self.state.iter().all(|x| x.abs() <= 1e-20)
        {
            self.reset();
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Snapshot {
    pub coefficients: Coefficients,
    pub revision: u64,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Update {
    pub snapshot: Snapshot,
    pub blending: bool,
    pub applied: bool,
}

pub struct Processor {
    front: Cascade,
    back: Cascade,
    sample_rate: u32,
    front_revision: u64,
    blend_revision: u64,
    blending: bool,
    blend_pos: usize,
    scratch: Vec<f32>,
}
impl Processor {
    pub fn new(channels: usize, sample_rate: u32) -> Result<Self> {
        let front = Cascade::new(channels)?;
        let back = Cascade::new(channels)?;
        let length = channels
            .max(1)
            .checked_mul(CHUNK_FRAMES)
            .filter(|&n| n <= isize::MAX as usize / size_of::<f32>())
            .ok_or(Error::InvalidArgument("HpTF: scratch length overflow"))?;
        let mut result = Self {
            front,
            back,
            sample_rate,
            front_revision: 0,
            blend_revision: 0,
            blending: false,
            blend_pos: 0,
            scratch: vec![0.0; length],
        };
        result.front.coefficients.sample_rate = sample_rate;
        Ok(result)
    }
    pub fn channels(&self) -> usize {
        self.front.channels()
    }
    pub fn update(&self, applied: bool) -> Update {
        Update {
            snapshot: Snapshot {
                coefficients: self.front.coefficients(),
                revision: self.front_revision,
            },
            blending: self.blending,
            applied,
        }
    }
    fn validate_target(&self, target: &Snapshot) -> Result<()> {
        target.coefficients.validate()?;
        if target.coefficients.sample_rate != self.sample_rate {
            return Err(Error::InvalidArgument("HpTF: target sample rate mismatch"));
        }
        Ok(())
    }
    pub fn reset(&mut self, target: Option<Snapshot>) -> Result<Update> {
        if let Some(target) = target {
            self.validate_target(&target)?;
            self.front.coefficients = target.coefficients;
            self.front_revision = target.revision;
        } else if self.blending {
            std::mem::swap(&mut self.front, &mut self.back);
            self.front_revision = self.blend_revision;
        }
        self.front.reset();
        self.back.reset();
        self.blending = false;
        self.blend_pos = 0;
        Ok(self.update(true))
    }
    pub fn process(&mut self, samples: &mut [f32], target: Option<Snapshot>) -> Result<Update> {
        self.front.validate_buffer(samples.len())?;
        if let Some(target) = &target {
            self.validate_target(target)?;
            if self.blending {
                return Err(Error::InvalidArgument("HpTF: transition already active"));
            }
        }
        if samples.is_empty() || self.channels() == 0 {
            return Ok(self.update(false));
        }
        if let Some(target) = target {
            self.back.coefficients = target.coefficients;
            self.back.reset();
            self.blend_revision = target.revision;
            self.blend_pos = 0;
            self.blending = true;
        }
        let channels = self.channels();
        let mut applied = false;
        for chunk in samples.chunks_mut(CHUNK_FRAMES * channels) {
            if !self.blending {
                self.front.process_validated(chunk);
                continue;
            }
            let scratch = &mut self.scratch[..chunk.len()];
            scratch.copy_from_slice(chunk);
            self.front.process_validated(chunk);
            self.back.process_validated(scratch);
            for (frame, (old, new)) in chunk
                .chunks_mut(channels)
                .zip(scratch.chunks(channels))
                .enumerate()
            {
                let t = ((self.blend_pos + frame) as f64 / BLEND_FRAMES as f64).min(1.0) as f32;
                for (old, &new) in old.iter_mut().zip(new) {
                    *old = (*old * (1.0 - t)) + (new * t);
                }
            }
            self.blend_pos += chunk.len() / channels;
            if self.blend_pos >= BLEND_FRAMES {
                std::mem::swap(&mut self.front, &mut self.back);
                self.front_revision = self.blend_revision;
                self.blending = false;
                self.blend_pos = 0;
                applied = true;
            }
        }
        Ok(self.update(applied))
    }
}
