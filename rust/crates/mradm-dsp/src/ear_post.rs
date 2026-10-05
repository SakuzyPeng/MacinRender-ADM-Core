//! EAR FIR decorrelation and direct-bus compensation. Filter design stays in libear.
use crate::{Complex32, Error, Result, fft::RealFft};
use std::sync::Arc;

pub const TAPS: usize = 512;
pub const DELAY: usize = 255;
const OVERLAP: usize = TAPS - 1;

fn invalid() -> Error {
    Error::InvalidArgument("Invalid EAR post-processing configuration or buffers")
}
fn checked_samples(channels: usize, frames: usize) -> Result<usize> {
    channels
        .checked_mul(frames)
        .filter(|&n| n <= isize::MAX as usize / size_of::<Complex32>())
        .ok_or_else(invalid)
}

/// Immutable channel-major FIRs. Shared across output instances, never modified by process/reset.
pub struct FilterBank {
    channels: usize,
    firs: Vec<f32>,
}
impl FilterBank {
    pub fn new(channels: usize, firs: &[f32], delay: usize) -> Result<Self> {
        if channels == 0
            || delay != DELAY
            || firs.len() != checked_samples(channels, TAPS)?
            || firs.iter().any(|v| !v.is_finite())
        {
            return Err(invalid());
        }
        Ok(Self {
            channels,
            firs: firs.to_vec(),
        })
    }
    pub fn channels(&self) -> usize {
        self.channels
    }
}

pub struct Processor {
    bank: Arc<FilterBank>,
    max_frames: usize,
    fft: RealFft,
    filters: Vec<Complex32>,
    overlap: Vec<f32>,
    delay: Vec<f32>,
    delay_pos: usize,
    input: Vec<f32>,
    source_fd: Vec<Complex32>,
    filtered_fd: Vec<Complex32>,
    output: Vec<f32>,
}
impl Processor {
    pub fn new(bank: Arc<FilterBank>, max_frames: usize) -> Result<Self> {
        if max_frames == 0 {
            return Err(invalid());
        }
        checked_samples(bank.channels, max_frames)?;
        let length = max_frames
            .checked_add(OVERLAP)
            .and_then(usize::checked_next_power_of_two)
            .ok_or_else(invalid)?;
        // Match the shared FFT's supported range before allocating channel-dependent storage.
        if length > (1 << 24) {
            return Err(invalid());
        }
        let mut fft = RealFft::new(length)?;
        let bins = fft.bins();
        let mut filters = vec![Complex32::default(); checked_samples(bank.channels, bins)?];
        let mut input = vec![0.; length];
        for (channel, filter) in filters.chunks_exact_mut(bins).enumerate() {
            input[..TAPS].copy_from_slice(&bank.firs[channel * TAPS..(channel + 1) * TAPS]);
            fft.forward(&input, filter)?;
            if filter
                .iter()
                .any(|v| !v.re.is_finite() || !v.im.is_finite())
            {
                return Err(invalid());
            }
        }
        let overlap = vec![0.; checked_samples(bank.channels, OVERLAP)?];
        let delay = vec![0.; checked_samples(bank.channels, DELAY)?];
        Ok(Self {
            bank,
            max_frames,
            fft,
            filters,
            overlap,
            delay,
            delay_pos: 0,
            input,
            source_fd: vec![Complex32::default(); bins],
            filtered_fd: vec![Complex32::default(); bins],
            output: vec![0.; length],
        })
    }
    pub fn reset(&mut self) {
        self.overlap.fill(0.);
        self.delay.fill(0.);
        self.delay_pos = 0;
    }
    /// Direct is replaced by delayed direct + decorrelated diffuse. Only the requested prefix is written.
    pub fn process(&mut self, direct: &mut [f32], diffuse: &[f32], frames: usize) -> Result<()> {
        let channels = self.bank.channels;
        let samples = checked_samples(channels, frames)?;
        if frames > self.max_frames
            || !direct.len().is_multiple_of(channels)
            || !diffuse.len().is_multiple_of(channels)
            || direct.len() < samples
            || diffuse.len() < samples
        {
            return Err(invalid());
        }
        if frames == 0 {
            return Ok(());
        }
        let bins = self.fft.bins();
        for channel in 0..channels {
            self.input.fill(0.);
            for frame in 0..frames {
                self.input[frame] = diffuse[frame * channels + channel];
            }
            self.fft.forward(&self.input, &mut self.source_fd)?;
            for (bin, value) in self.filtered_fd.iter_mut().enumerate() {
                *value = self.source_fd[bin] * self.filters[channel * bins + bin];
            }
            self.fft.inverse(&self.filtered_fd, &mut self.output)?;
            let overlap = &mut self.overlap[channel * OVERLAP..(channel + 1) * OVERLAP];
            let delay = &mut self.delay[channel * DELAY..(channel + 1) * DELAY];
            let mut position = self.delay_pos;
            for frame in 0..frames {
                let wet = self.output[frame] + overlap.get(frame).copied().unwrap_or(0.);
                let sample = frame * channels + channel;
                let dry = delay[position];
                delay[position] = direct[sample];
                direct[sample] = dry + wet;
                position += 1;
                if position == DELAY {
                    position = 0;
                }
            }
            // Forward iteration reads a later, still untouched old tail slot.
            // With normal large blocks there is no residual; short blocks retain unconsumed history.
            for i in 0..OVERLAP {
                overlap[i] = if frames + i < OVERLAP {
                    self.output[frames + i] + overlap[frames + i]
                } else {
                    self.output[frames + i]
                };
            }
        }
        self.delay_pos = (self.delay_pos + frames) % DELAY;
        Ok(())
    }
}
