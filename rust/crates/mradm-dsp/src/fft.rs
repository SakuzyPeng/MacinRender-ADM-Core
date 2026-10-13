//! Stateful real FFT with a non-destructive input and a normalized inverse.
//!
//! The patched default RustFFT planner is explicitly scalar (ADR 0015), even when native
//! kernels are available. Only an explicit Native policy opts into platform SIMD/FMA.
//! The local RustFFT patch batches independent radix-4 columns for automatic vectorization;
//! each column retains the scalar planner's coefficients, expression tree and signed zeros.
use crate::{Complex32, Error, Result};
use realfft::{ComplexToReal, ComplexToRealEven, RealFftPlanner, RealToComplex, RealToComplexEven};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FftPolicy {
    #[default]
    Exact,
    Native,
}

pub struct RealFft {
    forward: Arc<dyn RealToComplex<f32>>,
    inverse: Arc<dyn ComplexToReal<f32>>,
    real: Vec<f32>,
    complex: Vec<Complex32>,
    scratch: Vec<Complex32>,
}

impl RealFft {
    pub fn new(length: usize) -> Result<Self> {
        Self::with_policy(length, FftPolicy::Exact)
    }

    pub fn with_policy(length: usize, policy: FftPolicy) -> Result<Self> {
        if length < 2 || !length.is_power_of_two() || length > (1 << 24) {
            return Err(Error::InvalidArgument(
                "FFT length must be a power of two in [2, 16777216]",
            ));
        }
        let (forward, inverse): (Arc<dyn RealToComplex<f32>>, Arc<dyn ComplexToReal<f32>>) =
            match policy {
                FftPolicy::Exact => {
                    let mut planner = RealFftPlanner::new();
                    (
                        planner.plan_fft_forward(length),
                        planner.plan_fft_inverse(length),
                    )
                }
                FftPolicy::Native => {
                    let mut planner = rustfft::FftPlanner::new_native();
                    (
                        Arc::new(RealToComplexEven::new(length, &mut planner)),
                        Arc::new(ComplexToRealEven::new(length, &mut planner)),
                    )
                }
            };
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

    /// Transform a prefix followed by positive zeros without an intermediate
    /// time buffer. The FFT destroys its workspace, so refill the entire suffix
    /// on every call, including when the preceding prefix was longer.
    pub(crate) fn forward_padded(&mut self, input: &[f32], output: &mut [Complex32]) -> Result<()> {
        if input.len() > self.len() || output.len() != self.bins() {
            return Err(Error::InvalidArgument("FFT buffer length mismatch"));
        }
        self.real[..input.len()].copy_from_slice(input);
        self.real[input.len()..].fill(0.0);
        crate::diagnostics::f32("10-fft-input.f32", &self.real);
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
    fn default_planner_remains_scalar_with_native_kernels_available() {
        for size in [32, 512, 4096] {
            let input: Vec<_> = (0..size)
                .map(|i| {
                    Complex32::new(
                        ((i * 73 % 127) as f32 - 63.) / 127.,
                        ((i * 19 % 61) as f32 - 30.) / 61.,
                    )
                })
                .collect();
            let mut expected = input.clone();
            rustfft::FftPlannerScalar::new()
                .plan_fft_forward(size)
                .process(&mut expected);
            let mut actual = input;
            rustfft::FftPlanner::new()
                .plan_fft_forward(size)
                .process(&mut actual);
            assert!(
                actual
                    .iter()
                    .zip(&expected)
                    .all(|(a, b)| a.re.to_bits() == b.re.to_bits()
                        && a.im.to_bits() == b.im.to_bits())
            );
            let input64: Vec<_> = expected
                .iter()
                .map(|v| rustfft::num_complex::Complex64::new(v.re as f64, v.im as f64))
                .collect();
            let mut expected64 = input64.clone();
            rustfft::FftPlannerScalar::new()
                .plan_fft_inverse(size)
                .process(&mut expected64);
            let mut actual64 = input64;
            rustfft::FftPlanner::new()
                .plan_fft_inverse(size)
                .process(&mut actual64);
            assert!(
                actual64
                    .iter()
                    .zip(&expected64)
                    .all(|(a, b)| a.re.to_bits() == b.re.to_bits()
                        && a.im.to_bits() == b.im.to_bits())
            );
        }
    }

    #[test]
    fn native_and_exact_plans_coexist_without_changing_exact_output() {
        for size in [2, 32, 512, 4096, 32768] {
            let input: Vec<_> = (0..size)
                .map(|i| ((i * 73 % 127) as f32 - 63.) / 127.)
                .collect();
            let mut exact = RealFft::new(size).unwrap();
            let mut native = RealFft::with_policy(size, FftPolicy::Native).unwrap();
            let mut before = vec![Complex32::default(); exact.bins()];
            let mut after = before.clone();
            let mut fast = before.clone();
            exact.forward(&input, &mut before).unwrap();
            native.forward(&input, &mut fast).unwrap();
            let mut roundtrip = vec![0.; size];
            native.inverse(&fast, &mut roundtrip).unwrap();
            assert!(
                input
                    .iter()
                    .zip(roundtrip)
                    .all(|(a, b)| (*a - b).abs() < 2e-6)
            );
            exact.forward(&input, &mut after).unwrap();
            assert!(
                before
                    .iter()
                    .zip(after)
                    .all(|(a, b)| a.re.to_bits() == b.re.to_bits()
                        && a.im.to_bits() == b.im.to_bits())
            );
        }
    }

    #[test]
    fn padded_forward_matches_explicit_zeros_bit_for_bit() {
        for size in [2, 32, 512, 4096, 8192] {
            let mut padded = RealFft::new(size).unwrap();
            let mut reference = RealFft::new(size).unwrap();
            let input: Vec<_> = (0..size)
                .map(|i| match i % 7 {
                    0 => -0.0,
                    1 => f32::from_bits(1),
                    _ => ((i * 73 % 127) as f32 - 63.0) / 127.0,
                })
                .collect();
            let before: Vec<_> = input.iter().map(|v| v.to_bits()).collect();
            let mut actual = vec![Complex32::default(); padded.bins()];
            let mut expected = actual.clone();
            // Reuse a destroyed workspace with both increasing and decreasing
            // support, including an empty prefix and a partial final partition.
            for count in [size, 1, size / 2, 0, size - 1, size / 2 + 1, 0] {
                let mut explicit = vec![0.0; size];
                explicit[..count].copy_from_slice(&input[..count]);
                reference.forward(&explicit, &mut expected).unwrap();
                padded.forward_padded(&input[..count], &mut actual).unwrap();
                for (a, b) in actual.iter().zip(&expected) {
                    assert_eq!(
                        (a.re.to_bits(), a.im.to_bits()),
                        (b.re.to_bits(), b.im.to_bits())
                    );
                }
            }
            assert_eq!(
                before,
                input.iter().map(|v| v.to_bits()).collect::<Vec<_>>()
            );
            assert!(
                padded
                    .forward_padded(&vec![0.0; size + 1], &mut actual)
                    .is_err()
            );
            assert!(padded.forward_padded(&input, &mut actual[1..]).is_err());
        }
    }

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
