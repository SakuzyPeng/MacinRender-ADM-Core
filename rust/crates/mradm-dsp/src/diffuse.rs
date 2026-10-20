//! Project's eight-tap mono diffuse delay and live input gain/diffuse ramps.
use crate::{Error, Result};

pub const DELAY_LENGTH: usize = 32;

#[derive(Clone, Copy)]
pub struct DiffuseDelay {
    delay: [f32; DELAY_LENGTH * 2],
    position: usize,
    step: usize,
}

impl Default for DiffuseDelay {
    fn default() -> Self {
        Self {
            delay: [0.; DELAY_LENGTH * 2],
            position: 0,
            step: 1,
        }
    }
}

impl DiffuseDelay {
    /// Preserve the eight physical delays for a 48/96 kHz room session.
    pub fn with_sample_rate(sample_rate: u32) -> Result<Self> {
        crate::room::validate_sample_rate(sample_rate)?;
        Ok(Self {
            step: (sample_rate / 48000) as usize,
            ..Self::default()
        })
    }
    pub fn reset(&mut self) {
        self.delay.fill(0.);
        self.position = 0;
    }

    fn next(&mut self, input: f32) -> f32 {
        const OFFSETS: [usize; 8] = [3, 7, 11, 17, 19, 23, 29, 31];
        const POLARITY: [f32; 8] = [1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0, -1.0];
        let length = DELAY_LENGTH * self.step;
        // Construction permits only 48/96 kHz: the ring has 32 or 64 entries.
        // Integer wrap changes no sample, tap order or floating-point operation.
        let mask = length - 1;
        self.delay[self.position] = input;
        let mut sum = 0.0;
        for (offset, polarity) in OFFSETS.into_iter().zip(POLARITY) {
            sum += self.delay[(self.position + length - offset * self.step) & mask] * polarity;
        }
        self.position = (self.position + 1) & mask;
        sum * 0.35355339
    }

    pub fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        if input.len() != output.len() || input.iter().any(|v| !v.is_finite()) {
            return Err(Error::InvalidArgument("Invalid diffuse input or output"));
        }
        for (sample, value) in input.iter().zip(output) {
            *value = self.next(*sample);
        }
        Ok(())
    }

    /// In-place input-timeline mix. The endpoint belongs to the next segment,
    /// and muted segments feed silence into the delay while draining old taps.
    pub fn mix(&mut self, input: &mut [f32], gain: [f32; 2], diffuse: [f32; 2]) -> Result<()> {
        if input
            .iter()
            .chain(gain.iter())
            .chain(diffuse.iter())
            .any(|v| !v.is_finite())
        {
            return Err(Error::InvalidArgument(
                "Diffuse samples and controls must be finite",
            ));
        }
        let diffuse = diffuse.map(|v| v.clamp(0.0, 1.0));
        let frames = input.len();
        for (i, sample) in input.iter_mut().enumerate() {
            if gain == [0.0, 0.0] {
                *sample = 0.0;
            }
            let delayed = self.next(*sample);
            let alpha = i as f32 / frames as f32;
            let g = gain[0] + (gain[1] - gain[0]) * alpha;
            let d = diffuse[0] + (diffuse[1] - diffuse[0]) * alpha;
            *sample = g * (*sample * (1.0 - d) + delayed * d);
        }
        Ok(())
    }
}
