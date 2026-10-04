//! Prepared, immutable binaural spectra and the project's two HRTF lookups.
//! Quantized lookup preserves the one-degree offline grid (including its +180
//! endpoint). Continuous lookup blends the responses of four adjacent cells.
use crate::{
    Complex32, Error, Result,
    fft::RealFft,
    hrtf::{AZIMUTHS, Grid},
};
use std::sync::Arc;

#[derive(Clone, Copy)]
pub enum Lookup {
    Quantized,
    Continuous,
}

/// Optional offline diagnostic buffers, all in bin/ear order. This is filled
/// by the same kernel as production interpolation, not a second implementation.
pub struct Trace<'a> {
    pub magnitudes: &'a mut [f32],  // three neighbours per complex output
    pub complex_sum: &'a mut [f32], // re/im per complex output
    pub scales: &'a mut [f32],      // weighted magnitude / complex magnitude
}

fn coordinates(azimuth: f32, elevation: f32) -> Result<(f32, f32)> {
    if !azimuth.is_finite() || !elevation.is_finite() {
        return Err(Error::InvalidArgument("Nonfinite HRTF direction"));
    }
    // Keep the previous f32 addition/remainder order, including seam rounding.
    let mut azimuth = (azimuth + 180.0) % 360.0;
    if azimuth < 0.0 {
        azimuth += 360.0;
    }
    Ok((azimuth, (elevation + 90.0).clamp(0.0, 180.0)))
}

pub fn grid_index(azimuth: f32, elevation: f32) -> Result<usize> {
    let (azimuth, elevation) = coordinates(azimuth, elevation)?;
    Ok(elevation.round() as usize * AZIMUTHS + (azimuth.round() as usize).min(AZIMUTHS - 1))
}

struct GridBin {
    value: Complex32,
    magnitudes: [f32; 3],
    sum: Complex32,
    magnitude: f32,
    complex_magnitude: f32,
}

/// Concurrent queries are safe: only caller-owned output buffers are written.
/// FFT plans and transform scratch are dropped when preparation completes.
pub struct Filters {
    grid: Arc<Grid>,
    bins: usize,
    spectra: Vec<Complex32>, // [bin][ear][measurement]
    magnitudes: Vec<f32>,    // live-only cache; offline computes on demand
}

impl Filters {
    pub fn new(
        grid: Arc<Grid>,
        impulses: &[f32],
        taps: usize,
        fft_length: usize,
        cache_magnitudes: bool,
    ) -> Result<Self> {
        let directions = grid.direction_count();
        let input_length = directions.checked_mul(2).and_then(|n| n.checked_mul(taps));
        let spectrum_length = directions
            .checked_mul(2)
            .and_then(|n| n.checked_mul(fft_length / 2 + 1));
        if taps == 0
            || taps > fft_length
            || input_length != Some(impulses.len())
            || spectrum_length.is_none_or(|n| n > isize::MAX as usize / size_of::<Complex32>())
            || impulses.iter().any(|x| !x.is_finite())
        {
            return Err(Error::InvalidArgument("Invalid HRTF impulse buffers"));
        }
        let mut fft = RealFft::new(fft_length)?;
        let bins = fft.bins();
        let mut spectra = vec![Complex32::default(); spectrum_length.unwrap()];
        let mut signal = vec![0.0; fft_length];
        let mut spectrum = vec![Complex32::default(); bins];
        for direction in 0..directions {
            for ear in 0..2 {
                let offset = (direction * 2 + ear) * taps;
                signal[..taps].copy_from_slice(&impulses[offset..offset + taps]);
                fft.forward(&signal, &mut spectrum)?;
                for (bin, value) in spectrum.iter().copied().enumerate() {
                    if !value.re.is_finite()
                        || !value.im.is_finite()
                        || !value.re.hypot(value.im).is_finite()
                    {
                        return Err(Error::RenderFailed("Nonfinite prepared HRTF spectrum"));
                    }
                    spectra[(bin * 2 + ear) * directions + direction] = value;
                }
            }
        }
        let magnitudes = if cache_magnitudes {
            spectra.iter().map(|h| h.re.hypot(h.im)).collect()
        } else {
            Vec::new()
        };
        Ok(Self {
            grid,
            bins,
            spectra,
            magnitudes,
        })
    }

    pub fn output_len(&self) -> usize {
        self.bins * 4
    }
    pub fn spectrum_len(&self) -> usize {
        self.spectra.len() * 2
    }
    pub fn grid(&self) -> &Grid {
        &self.grid
    }

    /// Includes shared grid payload conservatively in each bank's cache budget;
    /// excludes allocator and Arc control-block overhead.
    pub fn storage_bytes(&self) -> usize {
        size_of::<Self>()
            + self.spectra.capacity() * size_of::<Complex32>()
            + self.magnitudes.capacity() * size_of::<f32>()
            + self.grid.storage_bytes()
    }

    /// Explicit snapshot for diagnostics/tests only. Normal rendering never
    /// copies the full measurement spectrum across the language boundary.
    pub fn copy_spectra(&self, output: &mut [f32]) -> Result<()> {
        if output.len() != self.spectrum_len() {
            return Err(Error::InvalidArgument(
                "Invalid HRTF spectrum snapshot length",
            ));
        }
        for (pair, h) in output.as_chunks_mut::<2>().0.iter_mut().zip(&self.spectra) {
            pair.copy_from_slice(&[h.re, h.im]);
        }
        Ok(())
    }

    // Magnitude comes from weighted |H|, phase from weighted complex H. This
    // suppresses phase-cancellation combs but is not an interpolated ITD model.
    // Near-zero complex sums retain the previous real-positive fallback.
    fn grid_bin(&self, grid: usize, bin: usize, ear: usize) -> GridBin {
        let mut magnitudes = [0.0; 3];
        let mut magnitude = 0.0;
        let mut sum = Complex32::default();
        for (k, m) in magnitudes.iter_mut().enumerate() {
            let gain = self.grid.weights()[grid * 3 + k];
            let direction = self.grid.indices()[grid * 3 + k] as usize;
            let index = (bin * 2 + ear) * self.grid.direction_count() + direction;
            let h = self.spectra[index];
            *m = if self.magnitudes.is_empty() {
                h.re.hypot(h.im)
            } else {
                self.magnitudes[index]
            };
            magnitude += gain * *m;
            sum += h * gain;
        }
        let complex_magnitude = sum.re.hypot(sum.im);
        let value = if complex_magnitude > 1e-9 {
            sum * (magnitude / complex_magnitude)
        } else {
            Complex32::new(magnitude, 0.0)
        };
        GridBin {
            value,
            magnitudes,
            sum,
            magnitude,
            complex_magnitude,
        }
    }

    pub fn query(
        &self,
        azimuth: f32,
        elevation: f32,
        lookup: Lookup,
        output: &mut [f32],
        mut trace: Option<Trace<'_>>,
    ) -> Result<()> {
        let (az, el) = coordinates(azimuth, elevation)?;
        let complex_length = self.output_len() / 2;
        if output.len() != self.output_len()
            || trace.as_ref().is_some_and(|t| {
                !matches!(lookup, Lookup::Quantized)
                    || t.magnitudes.len() != complex_length * 3
                    || t.complex_sum.len() != self.output_len()
                    || t.scales.len() != self.output_len()
            })
        {
            return Err(Error::InvalidArgument("Invalid HRTF query buffers"));
        }
        match lookup {
            Lookup::Quantized => {
                let grid = grid_index(azimuth, elevation)?;
                for bin in 0..self.bins {
                    for ear in 0..2 {
                        let h = self.grid_bin(grid, bin, ear);
                        let index = bin * 2 + ear;
                        output[index * 2..index * 2 + 2].copy_from_slice(&[h.value.re, h.value.im]);
                        if let Some(t) = &mut trace {
                            t.magnitudes[index * 3..index * 3 + 3].copy_from_slice(&h.magnitudes);
                            t.complex_sum[index * 2..index * 2 + 2]
                                .copy_from_slice(&[h.sum.re, h.sum.im]);
                            t.scales[index * 2..index * 2 + 2]
                                .copy_from_slice(&[h.magnitude, h.complex_magnitude]);
                        }
                    }
                }
            }
            Lookup::Continuous => {
                let (az0, el0) = (az.floor() as usize, el.floor() as usize);
                let (az1, el1) = ((az0 + 1) % 360, (el0 + 1).min(180));
                let (a, e) = (az - az0 as f32, el - el0 as f32);
                let corners = [
                    el0 * AZIMUTHS + az0,
                    el0 * AZIMUTHS + az1,
                    el1 * AZIMUTHS + az0,
                    el1 * AZIMUTHS + az1,
                ];
                let weights = [(1.0 - a) * (1.0 - e), a * (1.0 - e), (1.0 - a) * e, a * e];
                output.fill(0.0);
                for (grid, weight) in corners.into_iter().zip(weights) {
                    if weight == 0.0 {
                        continue;
                    }
                    for bin in 0..self.bins {
                        for ear in 0..2 {
                            let h = self.grid_bin(grid, bin, ear).value;
                            let index = (bin * 2 + ear) * 2;
                            output[index] += weight * h.re;
                            output[index + 1] += weight * h.im;
                        }
                    }
                }
            }
        }
        Ok(())
    }
}
