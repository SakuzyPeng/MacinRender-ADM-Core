//! Fixed-rate interleaved audio resampling. Prepared buffers, explicit consumption,
//! compensated startup delay and an exact rational duration at end of input.
use crate::{Error, Result};
use rubato::{
    Async, FixedAsync, Resampler as _, Resizable, SincInterpolationParameters,
    SincInterpolationType, WindowFunction, audioadapter_buffers::direct::InterleavedSlice,
};

const CHUNK: usize = 1024;
const BASE_TAPS: usize = 256;

#[derive(Debug, Default, Clone, Copy)]
pub struct Progress {
    pub input_frames: usize,
    pub output_frames: usize,
}

pub struct Resampler {
    channels: usize,
    input_rate: u32,
    output_rate: u32,
    inner: Option<Async<f32>>,
    scratch: Vec<f32>,
    zeros: Vec<f32>,
    pending_start: usize,
    pending_end: usize,
    initial_delay: usize,
    skip: usize,
    input_frames: u64,
    output_frames: u64,
    end: Option<u64>,
}

impl Resampler {
    pub fn new(channels: usize, input_rate: u32, output_rate: u32) -> Result<Self> {
        if !(1..=64).contains(&channels) || input_rate == 0 || output_rate == 0 {
            return Err(Error::InvalidArgument(
                "Invalid resampler channels or sample rates",
            ));
        }
        let ratio = output_rate as f64 / input_rate as f64;
        if !(1.0 / 256.0..=256.0).contains(&ratio) {
            return Err(Error::Unsupported(
                "Resampling ratio must be in [1/256, 256]",
            ));
        }
        let mut initial_delay = 0;
        let inner = if input_rate == output_rate {
            None
        } else {
            // Extend the filter at low ratios to retain the same normalized
            // transition width and stopband rejection during downsampling.
            let taps = ((BASE_TAPS as f64 / ratio.min(1.0)).ceil() as usize).div_ceil(8) * 8;
            let parameters = SincInterpolationParameters {
                sinc_len: taps,
                f_cutoff: Some(0.94),
                oversampling_factor: 128,
                interpolation: SincInterpolationType::Cubic,
                window: WindowFunction::BlackmanHarris2,
            };
            // Rubato 5 starts by advancing one output step; its reversed sinc
            // table contributes another 1/oversampling input sample. Round the
            // effective delay to the nearest output frame, rather than flooring
            // the nominal filter delay, which can discard an early HRIR impulse.
            // The independent phase/tap-zero tests guard this pinned convention.
            initial_delay =
                (taps as f64 * ratio / 2.0 - 1.0 - ratio / parameters.oversampling_factor as f64)
                    .round()
                    .max(0.0) as usize;
            Some(
                Async::new_sinc(ratio, 1.0, &parameters, CHUNK, channels, FixedAsync::Input)
                    .map_err(|_| Error::InvalidArgument("Could not prepare sinc resampler"))?,
            )
        };
        let output_capacity = inner.as_ref().map_or(0, |r| r.output_frames_max());
        Ok(Self {
            channels,
            input_rate,
            output_rate,
            inner,
            scratch: vec![0.0; output_capacity * channels],
            zeros: vec![0.0; CHUNK * channels],
            pending_start: 0,
            pending_end: 0,
            initial_delay,
            skip: initial_delay,
            input_frames: 0,
            output_frames: 0,
            end: None,
        })
    }
    pub fn channels(&self) -> usize {
        self.channels
    }
    pub fn output_length(&self, input_frames: usize) -> Result<usize> {
        let frames =
            (input_frames as u128 * self.output_rate as u128).div_ceil(self.input_rate as u128);
        usize::try_from(frames).map_err(|_| Error::InvalidArgument("Resampled length overflow"))
    }
    pub fn reset(&mut self) {
        if let Some(inner) = &mut self.inner {
            inner.reset();
        }
        self.pending_start = 0;
        self.pending_end = 0;
        self.skip = self.initial_delay;
        self.input_frames = 0;
        self.output_frames = 0;
        self.end = None;
    }

    fn validate_buffers(&self, input: &[f32], output: &[f32]) -> Result<()> {
        if !input.len().is_multiple_of(self.channels)
            || !output.len().is_multiple_of(self.channels)
            || output.is_empty()
        {
            return Err(Error::InvalidArgument(
                "Resampler requires complete frames and a nonempty output buffer",
            ));
        }
        if input.iter().any(|sample| !sample.is_finite()) {
            return Err(Error::InvalidArgument("Resampler input must be finite"));
        }
        Ok(())
    }

    fn copy_pending(&mut self, output: &mut [f32]) -> usize {
        let available = self.pending_end - self.pending_start;
        let limit = self.end.map_or(usize::MAX, |n| {
            (n - self.output_frames).min(usize::MAX as u64) as usize
        });
        let frames = available.min(output.len() / self.channels).min(limit);
        output[..frames * self.channels].copy_from_slice(
            &self.scratch
                [self.pending_start * self.channels..(self.pending_start + frames) * self.channels],
        );
        self.pending_start += frames;
        self.output_frames += frames as u64;
        frames
    }

    fn generated(&mut self, frames: usize) {
        let skip = self.skip.min(frames);
        self.skip -= skip;
        self.pending_start = skip;
        self.pending_end = frames;
    }

    /// Consume at most 1024 input frames or drain buffered output. Call again
    /// with the unconsumed input; an empty input drains pending output without
    /// ending the stream. Output capacity may be as small as one whole frame.
    pub fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<Progress> {
        self.validate_buffers(input, output)?;
        if self.end.is_some() {
            return Err(Error::InvalidArgument(
                "Resampler input follows end of stream",
            ));
        }
        if self.pending_start != self.pending_end {
            return Ok(Progress {
                input_frames: 0,
                output_frames: self.copy_pending(output),
            });
        }
        let frames = (input.len() / self.channels).min(CHUNK);
        let next_total = self
            .input_frames
            .checked_add(frames as u64)
            .ok_or(Error::InvalidArgument("Resampler input duration overflow"))?;
        if (next_total as u128 * self.output_rate as u128).div_ceil(self.input_rate as u128)
            > u64::MAX as u128
        {
            return Err(Error::InvalidArgument("Resampler output duration overflow"));
        }
        if frames == 0 {
            return Ok(Progress::default());
        }
        let Some(inner) = &mut self.inner else {
            let frames = frames.min(output.len() / self.channels);
            output[..frames * self.channels].copy_from_slice(&input[..frames * self.channels]);
            self.input_frames += frames as u64;
            self.output_frames += frames as u64;
            return Ok(Progress {
                input_frames: frames,
                output_frames: frames,
            });
        };
        inner
            .set_chunk_size(frames)
            .map_err(|_| Error::RenderFailed("Resampler chunk size failed"))?;
        let input = InterleavedSlice::new(&input[..frames * self.channels], self.channels, frames)
            .map_err(|_| Error::InvalidArgument("Invalid interleaved resampler input"))?;
        let output_capacity = self.scratch.len() / self.channels;
        let mut scratch =
            InterleavedSlice::new_mut(&mut self.scratch, self.channels, output_capacity)
                .map_err(|_| Error::RenderFailed("Invalid prepared resampler output"))?;
        let (consumed, generated) = inner
            .process_into_buffer(&input, &mut scratch, None)
            .map_err(|_| Error::RenderFailed("Sinc resampling failed"))?;
        if consumed != frames {
            return Err(Error::RenderFailed("Unexpected resampler consumption"));
        }
        self.input_frames = next_total;
        self.generated(generated);
        Ok(Progress {
            input_frames: consumed,
            output_frames: self.copy_pending(output),
        })
    }

    /// Zero-extend and drain to ceil(total_input * output_rate / input_rate).
    /// Repeated calls after completion return zero. No fabricated output padding
    /// replaces the filter tail; every returned frame has passed through the sinc.
    pub fn finish(&mut self, output: &mut [f32]) -> Result<usize> {
        self.validate_buffers(&[], output)?;
        let target = (self.input_frames as u128 * self.output_rate as u128)
            .div_ceil(self.input_rate as u128);
        let target = u64::try_from(target)
            .map_err(|_| Error::InvalidArgument("Resampled duration overflow"))?;
        if self.output_frames > target {
            return Err(Error::RenderFailed("Resampler exceeded its timeline"));
        }
        self.end = Some(target);
        if self.output_frames == target {
            return Ok(0);
        }
        while self.pending_start == self.pending_end {
            let inner = self
                .inner
                .as_mut()
                .ok_or(Error::RenderFailed("Invalid bypass resampler tail"))?;
            inner
                .set_chunk_size(CHUNK)
                .map_err(|_| Error::RenderFailed("Resampler flush size failed"))?;
            let input = InterleavedSlice::new(&self.zeros, self.channels, CHUNK)
                .map_err(|_| Error::RenderFailed("Invalid resampler silence buffer"))?;
            let output_capacity = self.scratch.len() / self.channels;
            let mut scratch =
                InterleavedSlice::new_mut(&mut self.scratch, self.channels, output_capacity)
                    .map_err(|_| Error::RenderFailed("Invalid resampler flush buffer"))?;
            let (_, generated) = inner
                .process_into_buffer(&input, &mut scratch, None)
                .map_err(|_| Error::RenderFailed("Resampler flush failed"))?;
            self.generated(generated);
        }
        Ok(self.copy_pending(output))
    }
}
