//! Headphone compensation: quantized biquads, double state and linear swaps.
//! Control publication and applied-state mailboxes belong to the C++ adapter.
mod design;
mod parametric_eq;
use crate::{Error, Result};
pub use design::{design, design_stereo};
pub use parametric_eq::{ParseError, ParseErrorKind, Profile, parse as parse_parametric_eq};

pub const MAX_BANDS: usize = 32;
pub const BLEND_FRAMES: usize = 2048;
pub const CHUNK_FRAMES: usize = 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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
#[derive(Clone, Copy, Debug, PartialEq)]
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
    right: Option<Coefficients>,
    right_position: usize,
    channels: usize,
    state: Vec<f64>,
    // Frames since the last cleanup point; cleanup runs every CHUNK_FRAMES after reset.
    position: usize,
}
impl Cascade {
    pub fn new(channels: usize) -> Result<Self> {
        let count = channels
            .checked_mul(MAX_BANDS * 2)
            .filter(|&n| n <= isize::MAX as usize / size_of::<f64>())
            .ok_or(Error::InvalidArgument("HpTF: channel count overflow"))?;
        Ok(Self {
            coefficients: Coefficients::default(),
            right: None,
            right_position: 0,
            channels,
            state: vec![0.0; count],
            position: 0,
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
        self.right = None;
        Ok(())
    }
    pub fn reset(&mut self) {
        self.state.fill(0.0);
        self.position = 0;
        self.right_position = 0;
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
    /// Cleanup points lie on a fixed frame grid from the last reset, so the
    /// output does not depend on how callers split the stream.
    fn process_validated(&mut self, mut samples: &mut [f32]) {
        if self.channels == 0 {
            return;
        }
        if let Some(right) = self.right {
            let (left_state, right_state) = self.state.split_at_mut(MAX_BANDS * 2);
            Self::process_ear(
                samples,
                0,
                &self.coefficients,
                left_state,
                &mut self.position,
            );
            Self::process_ear(samples, 1, &right, right_state, &mut self.right_position);
            return;
        }
        while !samples.is_empty() {
            let frames = (CHUNK_FRAMES - self.position).min(samples.len() / self.channels);
            let (head, tail) = samples.split_at_mut(frames * self.channels);
            if !self.coefficients.is_bypass() {
                self.filter(head);
            }
            self.position += frames;
            if self.position == CHUNK_FRAMES {
                self.position = 0;
                self.cleanup();
            }
            samples = tail;
        }
    }
    fn filter_channel(
        samples: impl Iterator<Item = impl std::ops::DerefMut<Target = f32>>,
        coefficients: &Coefficients,
        state: &mut [f64],
    ) {
        let preamp = coefficients.preamp_gain as f64;
        let sections = &coefficients.sections[..coefficients.band_count];
        let state = state.as_chunks_mut::<2>().0;
        for mut sample in samples {
            let mut x = *sample as f64 * preamp;
            for (s, z) in sections.iter().zip(state.iter_mut()) {
                let y = (s.b0 as f64 * x) + z[0];
                z[0] = ((s.b1 as f64 * x) - (s.a1 as f64 * y)) + z[1];
                z[1] = (s.b2 as f64 * x) - (s.a2 as f64 * y);
                x = y;
            }
            *sample = x as f32;
        }
    }
    fn filter(&mut self, samples: &mut [f32]) {
        for (channel, state) in self
            .state
            .as_chunks_mut::<{ MAX_BANDS * 2 }>()
            .0
            .iter_mut()
            .enumerate()
        {
            Self::filter_channel(
                samples.iter_mut().skip(channel).step_by(self.channels),
                &self.coefficients,
                state,
            );
        }
    }
    fn process_ear(
        mut samples: &mut [f32],
        ear: usize,
        coefficients: &Coefficients,
        state: &mut [f64],
        position: &mut usize,
    ) {
        while !samples.is_empty() {
            let frames = (CHUNK_FRAMES - *position).min(samples.len() / 2);
            let (head, tail) = samples.split_at_mut(frames * 2);
            if !coefficients.is_bypass() {
                Self::filter_channel(head.iter_mut().skip(ear).step_by(2), coefficients, state);
            }
            *position += frames;
            if *position == CHUNK_FRAMES {
                *position = 0;
                Self::cleanup_state(state);
            }
            samples = tail;
        }
    }
    fn cleanup_state(state: &mut [f64]) {
        if state.iter().any(|x| !x.is_finite()) || state.iter().all(|x| x.abs() <= 1e-20) {
            state.fill(0.0);
        }
    }
    /// Legacy shared cascades retain their original cleanup scope and arithmetic.
    fn cleanup(&mut self) {
        Self::cleanup_state(&mut self.state);
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

/// A complete publication. `independent=false` is the legacy shared numerical path.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StereoSnapshot {
    pub coefficients: [Coefficients; 2],
    pub revision: u64,
    pub independent: bool,
}
impl From<Snapshot> for StereoSnapshot {
    fn from(value: Snapshot) -> Self {
        Self {
            coefficients: [value.coefficients; 2],
            revision: value.revision,
            independent: false,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StereoUpdate {
    pub snapshot: StereoSnapshot,
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
    /// Latest target received during a transition; it starts at the exact
    /// frame where that transition ends.
    queued: Option<StereoSnapshot>,
    preserve: [bool; 2],
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
            queued: None,
            preserve: [false; 2],
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
    pub fn stereo_update(&self, applied: bool) -> StereoUpdate {
        StereoUpdate {
            snapshot: StereoSnapshot {
                coefficients: [
                    self.front.coefficients,
                    self.front.right.unwrap_or(self.front.coefficients),
                ],
                revision: self.front_revision,
                independent: self.front.right.is_some(),
            },
            blending: self.blending,
            applied,
        }
    }
    fn validate_target(&self, target: &StereoSnapshot) -> Result<()> {
        for c in target.coefficients {
            c.validate()?;
        }
        if target.independent && self.channels() != 2 {
            return Err(Error::InvalidArgument("HpTF:独立双耳配置只适用于双声道"));
        }
        if target
            .coefficients
            .iter()
            .any(|c| c.sample_rate != self.sample_rate)
        {
            return Err(Error::InvalidArgument("HpTF: target sample rate mismatch"));
        }
        Ok(())
    }
    /// Clear both histories. The newest target wins: an explicit target, then a
    /// queued one, then the transition target.
    pub fn reset(&mut self, target: Option<Snapshot>) -> Result<Update> {
        self.reset_stereo(target.map(Into::into))?;
        Ok(self.update(true))
    }
    pub fn reset_stereo(&mut self, target: Option<StereoSnapshot>) -> Result<StereoUpdate> {
        if let Some(target) = &target {
            self.validate_target(target)?;
        }
        if let Some(target) = target.or(self.queued.take()) {
            self.front.coefficients = target.coefficients[0];
            self.front.right = target.independent.then_some(target.coefficients[1]);
            self.front_revision = target.revision;
        } else if self.blending {
            std::mem::swap(&mut self.front, &mut self.back);
            self.front_revision = self.blend_revision;
        }
        self.front.reset();
        self.back.reset();
        self.blending = false;
        self.blend_pos = 0;
        self.queued = None;
        Ok(self.stereo_update(true))
    }
    fn start(&mut self, target: StereoSnapshot) {
        self.back.coefficients = target.coefficients[0];
        self.back.right = target.independent.then_some(target.coefficients[1]);
        self.back.reset();
        self.preserve = [false; 2];
        if target.independent {
            if self.front.right.is_none() {
                self.front.right = Some(self.front.coefficients);
                self.front.right_position = self.front.position;
            }
            for (ear, old) in [self.front.coefficients, self.front.right.unwrap()]
                .into_iter()
                .enumerate()
            {
                let new = &target.coefficients[ear];
                self.preserve[ear] = old.preamp_gain == new.preamp_gain
                    && old.band_count == new.band_count
                    && old.sections[..old.band_count] == new.sections[..new.band_count];
                if self.preserve[ear] {
                    let range = ear * MAX_BANDS * 2..(ear + 1) * MAX_BANDS * 2;
                    self.back.state[range.clone()].copy_from_slice(&self.front.state[range]);
                    if ear == 0 {
                        self.back.position = self.front.position;
                    } else {
                        self.back.right_position = self.front.right_position;
                    }
                }
            }
        }
        self.blend_revision = target.revision;
        self.blend_pos = 0;
        self.blending = true;
    }
    /// A target starts a transition at the first frame of this call, or, while
    /// one is active, replaces any queued target and starts where it ends.
    pub fn process(&mut self, samples: &mut [f32], target: Option<Snapshot>) -> Result<Update> {
        let result = self.process_stereo(samples, target.map(Into::into))?;
        Ok(self.update(result.applied))
    }
    pub fn process_stereo(
        &mut self,
        samples: &mut [f32],
        target: Option<StereoSnapshot>,
    ) -> Result<StereoUpdate> {
        self.front.validate_buffer(samples.len())?;
        if let Some(target) = &target {
            self.validate_target(target)?;
        }
        if samples.is_empty() || self.channels() == 0 {
            return Ok(self.stereo_update(false));
        }
        if let Some(target) = target {
            if self.blending {
                self.queued = Some(target);
            } else {
                self.start(target);
            }
        }
        let channels = self.channels();
        let mut applied = false;
        let mut rest = samples;
        while !rest.is_empty() {
            // A transition ends exactly at BLEND_FRAMES, independent of call sizes.
            let limit = if self.blending {
                CHUNK_FRAMES.min(BLEND_FRAMES - self.blend_pos)
            } else {
                CHUNK_FRAMES
            };
            let (chunk, tail) = rest.split_at_mut(limit.min(rest.len() / channels) * channels);
            rest = tail;
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
                for (ear, (old, &new)) in old.iter_mut().zip(new).enumerate() {
                    if self.preserve.get(ear).copied().unwrap_or(false) {
                        continue;
                    }
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
                if let Some(next) = self.queued.take() {
                    self.start(next);
                }
            }
        }
        Ok(self.stereo_update(applied))
    }
}
