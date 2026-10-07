//! Stateful real FFT with a non-destructive input and a normalized inverse.
//!
//! The workspace builds RustFFT without its SIMD features, so `RealFftPlanner` always falls back
//! to the scalar planner: the algorithm depends only on the length and no FMA is used, which keeps
//! the result bit-identical across x86_64 and aarch64 (ADR 0015). Do not re-enable `avx`, `sse` or
//! `neon`; the consistency comparator rejects builds that resolve them.
use crate::{Complex32, Error, Result};
use realfft::{ComplexToReal, RealFftPlanner, RealToComplex};
use std::sync::Arc;

pub struct RealFft {
    forward: Arc<dyn RealToComplex<f32>>,
    inverse: Arc<dyn ComplexToReal<f32>>,
    real: Vec<f32>,
    complex: Vec<Complex32>,
    scratch: Vec<Complex32>,
}

impl RealFft {
    pub fn new(length: usize) -> Result<Self> {
        if length < 2 || !length.is_power_of_two() || length > (1 << 24) {
            return Err(Error::InvalidArgument(
                "FFT length must be a power of two in [2, 16777216]",
            ));
        }
        let mut planner = RealFftPlanner::new();
        let forward = planner.plan_fft_forward(length);
        let inverse = planner.plan_fft_inverse(length);
        let scratch_length = forward.get_scratch_len().max(inverse.get_scratch_len());
        Ok(Self {
            forward,
            inverse,
            real: vec![0.0; length],
            complex: vec![Complex32::default(); length / 2 + 1],
            scratch: vec![Complex32::default(); scratch_length],
        })
    }

    pub fn len(&self) -> usize {
        self.real.len()
    }
    pub fn is_empty(&self) -> bool {
        false
    }
    pub fn bins(&self) -> usize {
        self.complex.len()
    }

    pub fn forward(&mut self, input: &[f32], output: &mut [Complex32]) -> Result<()> {
        if input.len() != self.len() || output.len() != self.bins() {
            return Err(Error::InvalidArgument("FFT buffer length mismatch"));
        }
        self.real.copy_from_slice(input);
        crate::diagnostics::f32("10-fft-input.f32", input);
        self.forward
            .process_with_scratch(&mut self.real, output, &mut self.scratch)
            .map_err(|_| Error::RenderFailed("Forward FFT failed"))
    }

    pub fn inverse(&mut self, input: &[Complex32], output: &mut [f32]) -> Result<()> {
        if input.len() != self.bins() || output.len() != self.len() {
            return Err(Error::InvalidArgument("Inverse FFT buffer length mismatch"));
        }
        self.complex.copy_from_slice(input);
        self.inverse_inner(output)
    }

    fn inverse_inner(&mut self, output: &mut [f32]) -> Result<()> {
        // Real signals have purely real DC and Nyquist bins. Interpolated HRTFs can
        // carry tiny imaginary residue there; the previous real FFT ignored it.
        self.complex[0].im = 0.0;
        self.complex.last_mut().unwrap().im = 0.0;
        self.inverse
            .process_with_scratch(&mut self.complex, output, &mut self.scratch)
            .map_err(|_| Error::RenderFailed("Inverse FFT failed"))?;
        let scale = 1.0 / self.len() as f32;
        crate::diagnostics::f32("20-inverse-unscaled.f32", output);
        for sample in output {
            *sample *= scale;
        }
        Ok(())
    }

    /// FFI representation is pairs of floats; no assumptions about C++ complex layout
    /// enter Rust. Both conversion buffers are allocated with the plan.
    pub fn forward_interleaved(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        if input.len() != self.len() || output.len() != self.bins() * 2 {
            return Err(Error::InvalidArgument("FFT buffer length mismatch"));
        }
        self.real.copy_from_slice(input);
        self.forward
            .process_with_scratch(&mut self.real, &mut self.complex, &mut self.scratch)
            .map_err(|_| Error::RenderFailed("Forward FFT failed"))?;
        for (pair, value) in output.as_chunks_mut::<2>().0.iter_mut().zip(&self.complex) {
            pair[0] = value.re;
            pair[1] = value.im;
        }
        Ok(())
    }

    pub fn inverse_interleaved(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        if input.len() != self.bins() * 2 || output.len() != self.len() {
            return Err(Error::InvalidArgument("Inverse FFT buffer length mismatch"));
        }
        for (pair, value) in input.as_chunks::<2>().0.iter().zip(&mut self.complex) {
            *value = Complex32::new(pair[0], pair[1]);
        }
        self.inverse_inner(output)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_independent_double_dft_and_preserves_input() {
        for size in [2, 4, 16, 64] {
            let mut fft = RealFft::new(size).unwrap();
            let input: Vec<_> = (0..size)
                .map(|i| ((i * 73 % 127) as f32 - 63.0) / 127.0)
                .collect();
            let mut spectrum = vec![Complex32::default(); fft.bins()];
            fft.forward(&input, &mut spectrum).unwrap();
            for (bin, actual) in spectrum.iter().enumerate() {
                let mut reference = rustfft::num_complex::Complex64::default();
                for (n, value) in input.iter().enumerate() {
                    let angle = -std::f64::consts::TAU * bin as f64 * n as f64 / size as f64;
                    reference += rustfft::num_complex::Complex64::from_polar(*value as f64, angle);
                }
                let error =
                    (rustfft::num_complex::Complex64::new(actual.re as f64, actual.im as f64)
                        - reference)
                        .norm();
                assert!(
                    error < 1e-6 + 1e-5 * reference.norm(),
                    "size={size}, bin={bin}, error={error}"
                );
            }
            let before = spectrum.clone();
            let mut roundtrip = vec![0.0; size];
            fft.inverse(&spectrum, &mut roundtrip).unwrap();
            assert_eq!(before, spectrum);
            for (a, b) in input.iter().zip(roundtrip) {
                assert!((a - b).abs() < 1e-6);
            }
        }
    }

    #[test]
    fn production_lengths_roundtrip_and_reuse() {
        for size in [128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768] {
            let mut fft = RealFft::new(size).unwrap();
            let mut input = vec![0.0; size];
            input[size / 3] = 1.0;
            let mut spectrum = vec![0.0; 2 * fft.bins()];
            let mut output = vec![0.0; size];
            for _ in 0..3 {
                fft.forward_interleaved(&input, &mut spectrum).unwrap();
                fft.inverse_interleaved(&spectrum, &mut output).unwrap();
                for (a, b) in input.iter().zip(&output) {
                    assert!((a - b).abs() < 1e-6);
                }
            }
            assert!(
                fft.forward_interleaved(&input[..size - 1], &mut spectrum)
                    .is_err()
            );
        }
        assert!(RealFft::new(0).is_err());
        assert!(RealFft::new(3).is_err());
    }
}
