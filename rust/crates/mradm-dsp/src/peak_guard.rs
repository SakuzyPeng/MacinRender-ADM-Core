//! Device-bound linked stereo sample-peak protection, with bounded lookahead.
use crate::{Error, Result};

pub const PULL_FRAMES: usize = 4096;
pub const CEILING: f32 = 0.89125094;

#[derive(Clone)]
pub struct StereoPeakGuard {
    samples: Vec<f32>,
    peaks: Vec<f32>,
    attack_weights: Vec<f32>,
    read: usize,
    size: usize,
    gain: f32,
    release: f32,
}

impl StereoPeakGuard {
    pub fn new(sample_rate: u32) -> Result<Self> {
        if sample_rate == 0 {
            return Err(Error::InvalidArgument("Invalid peak guard sample rate"));
        }
        let lookahead = (sample_rate as usize / 200).max(1);
        let capacity = PULL_FRAMES + lookahead;
        let attack_weights = (0..=lookahead)
            .map(|i| 1. - i as f32 / lookahead as f32)
            .collect();
        Ok(Self {
            samples: vec![0.; capacity * 2],
            peaks: vec![0.; capacity],
            attack_weights,
            read: 0,
            size: 0,
            gain: 1.,
            release: 1. - (-1. / (0.1 * sample_rate as f32)).exp(),
        })
    }
    pub fn reset(&mut self) {
        self.read = 0;
        self.size = 0;
        self.gain = 1.;
    }
    pub fn lookahead_frames(&self) -> usize {
        self.attack_weights.len() - 1
    }
    pub fn buffered_frames(&self) -> usize {
        self.size
    }
    pub fn writable_frames(&self) -> usize {
        self.peaks.len() - self.size
    }
    pub fn readable_frames(&self, ended: bool) -> usize {
        if ended {
            self.size
        } else {
            self.size.saturating_sub(self.lookahead_frames())
        }
    }
    pub fn push(&mut self, stereo: &[f32]) -> Result<()> {
        if !stereo.len().is_multiple_of(2) || stereo.len() / 2 > self.writable_frames() {
            return Err(Error::InvalidArgument("Invalid peak guard input length"));
        }
        let mut write = (self.read + self.size) % self.peaks.len();
        for frame in stereo.as_chunks::<2>().0 {
            let left = if frame[0].is_finite() { frame[0] } else { 0. };
            let right = if frame[1].is_finite() { frame[1] } else { 0. };
            self.samples[write * 2] = left;
            self.samples[write * 2 + 1] = right;
            self.peaks[write] = left.abs().max(right.abs());
            write = (write + 1) % self.peaks.len();
        }
        self.size += stereo.len() / 2;
        Ok(())
    }
    fn next_gain(&mut self, volume: f32) -> f32 {
        let mut gain = self.gain + ((1. - self.gain) * self.release);
        let count = self.size.min(self.attack_weights.len());
        let mut sample = self.read;
        for ahead in 0..count {
            let peak = self.peaks[sample] * volume;
            if peak > CEILING {
                let required = CEILING / peak;
                gain = gain.min(1. - ((1. - required) * self.attack_weights[ahead]));
            }
            sample += 1;
            if sample == self.peaks.len() {
                sample = 0;
            }
        }
        self.gain = gain;
        gain
    }
    pub fn pop(&mut self, output: &mut [f32], volume: f32, ended: bool) -> Result<usize> {
        if !output.len().is_multiple_of(2) || !volume.is_finite() || !(0. ..=1.).contains(&volume) {
            return Err(Error::InvalidArgument(
                "Invalid peak guard output or volume",
            ));
        }
        let frames = (output.len() / 2).min(self.readable_frames(ended));
        for frame in output[..frames * 2].as_chunks_mut::<2>().0 {
            let gain = volume * self.next_gain(volume);
            frame[0] = self.samples[self.read * 2] * gain;
            frame[1] = self.samples[self.read * 2 + 1] * gain;
            self.read = (self.read + 1) % self.peaks.len();
            self.size -= 1;
        }
        Ok(frames)
    }
}
