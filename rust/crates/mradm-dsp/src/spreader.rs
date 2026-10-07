//! Two-ear optimal-mixing spreader. The signal model follows the SAF example
//! (copyright 2021 Leo McCormack, ISC; see assets/NOTICE.txt). Workspaces,
//! covariance, filterbank history and randomness belong to this instance.
use crate::{
    Complex32 as C, Error, Result,
    decorrelator::Decorrelator,
    filterbank::{self, BANDS, FRAME, FilterBank, HOP, SLOTS},
    geometry::{self, Vec3},
    mixing::{self, ComplexMatrix as M, RealMatrix as R},
};
use nalgebra::Vector2;

struct Source {
    position: Option<[f32; 3]>,
    cone: Vec<usize>,
    mean: Vec<[C; 2]>,
    shape: Vec<M>,
    center_energy: Vec<f32>,
    input_cov: Vec<M>,
    target_cov: Vec<M>,
    previous: Vec<M>,
    previous_residual: Vec<R>,
    decor: Decorrelator,
}
impl Source {
    fn new(rate: u32, freq: &[f32; BANDS], seed: u64, directions: usize) -> Self {
        Self {
            position: None,
            cone: Vec::with_capacity(directions),
            mean: vec![[C::default(); 2]; BANDS],
            shape: vec![M::zeros(); BANDS],
            center_energy: vec![0.; BANDS],
            input_cov: vec![M::zeros(); BANDS],
            target_cov: vec![M::zeros(); BANDS],
            previous: vec![M::zeros(); BANDS],
            previous_residual: vec![R::zeros(); BANDS],
            decor: Decorrelator::new(rate, freq, seed),
        }
    }
}

pub struct Spreader {
    directions: Vec<Vec3>,
    weights: Vec<f32>,
    hrtf: Vec<C>,
    frequencies: [f32; BANDS],
    sources: Vec<Source>,
    bank: FilterBank,
    input_hop: Vec<f32>,
    input_spectrum: Vec<C>,
    output_hop: [f32; HOP * 2],
    output_spectrum: [C; BANDS * 2],
    input_tf: Vec<C>,
    output_tf: Vec<C>,
    proto: Vec<C>,
    decor: Vec<C>,
}
impl Spreader {
    pub fn new(ir: &[f32], dirs: &[f32], taps: usize, rate: u32, seeds: &[u64]) -> Result<Self> {
        if seeds.is_empty() || seeds.len() > 8 || rate == 0 {
            return Err(Error::InvalidArgument("Invalid spreader configuration"));
        }
        let directions = geometry::portable_directions(dirs)?;
        let weights = geometry::voronoi_weights(&directions)?;
        let hrtf = filterbank::fir_coefficients(ir, directions.len(), taps)?;
        let frequencies = filterbank::frequencies(rate);
        let channels = seeds.len();
        let sources = seeds
            .iter()
            .map(|&seed| Source::new(rate, &frequencies, seed, directions.len()))
            .collect();
        let mut result = Self {
            directions,
            weights,
            hrtf,
            frequencies,
            sources,
            bank: FilterBank::new(channels, 2)?,
            input_hop: vec![0.; HOP * channels],
            input_spectrum: vec![C::default(); BANDS * channels],
            output_hop: [0.; HOP * 2],
            output_spectrum: [C::default(); BANDS * 2],
            input_tf: vec![C::default(); BANDS * channels * SLOTS],
            output_tf: vec![C::default(); BANDS * 2 * SLOTS],
            proto: vec![C::default(); BANDS * 2 * SLOTS],
            decor: vec![C::default(); BANDS * 2 * SLOTS],
        };
        for index in 0..channels {
            result.set_source(index, 0., 0., 0.)?;
        }
        Ok(result)
    }
    pub fn sources(&self) -> usize {
        self.sources.len()
    }

    pub fn set_source(&mut self, index: usize, mut az: f32, el: f32, spread: f32) -> Result<()> {
        if index >= self.sources.len() || !az.is_finite() || !el.is_finite() || !spread.is_finite()
        {
            return Err(Error::InvalidArgument("Invalid spreader source"));
        }
        if az > 180. {
            az -= 360.;
        }
        let position = [
            az.clamp(-180., 180.),
            el.clamp(-90., 90.),
            spread.clamp(0., 360.),
        ];
        let source = &mut self.sources[index];
        if source.position == Some(position) {
            return Ok(());
        }
        source.position = Some(position);
        source.cone.clear();
        let direction = geometry::portable_direction(position[0], position[1]);
        let threshold = libm::cos((position[2] as f64 * 0.5).to_radians());
        let mut center = 0;
        let mut maximum = f64::NEG_INFINITY;
        for (i, &point) in self.directions.iter().enumerate() {
            let dot = geometry::dot(point, direction).clamp(-1., 0.9999999);
            if dot > maximum {
                maximum = dot;
                center = i;
            }
            if dot >= threshold {
                source.cone.push(i);
            }
        }
        if source.cone.is_empty() {
            source.cone.push(center);
        }
        let count = self.directions.len();
        for band in 0..BANDS {
            let at = |dir: usize| {
                [
                    self.hrtf[(band * 2) * count + dir],
                    self.hrtf[(band * 2 + 1) * count + dir],
                ]
            };
            let direct = at(center);
            source.center_energy[band] = direct.iter().map(|h| h.norm_sqr()).sum();
            if self.frequencies[band] >= 16000. {
                source.mean[band] = direct;
                continue;
            }
            let mut mean = [C::default(); 2];
            let mut covariance = M::zeros();
            for &dir in &source.cone {
                let h = at(dir);
                for ear in 0..2 {
                    mean[ear] += h[ear];
                }
                for r in 0..2 {
                    for c in 0..2 {
                        covariance[(r, c)] += h[r] * h[c].conj() * self.weights[dir];
                    }
                }
            }
            source.mean[band] = mean.map(|v| v / source.cone.len() as f32);
            let trace = covariance.trace().re;
            source.shape[band] = covariance / C::new(trace + 2.23e-9, 0.);
        }
        Ok(())
    }

    /// Planar [source][512] -> planar [ear][512]. No allocation or locking.
    pub fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        let count = self.sources.len();
        if input.len() != count * FRAME
            || output.len() != FRAME * 2
            || input.iter().any(|v| !v.is_finite())
        {
            return Err(Error::InvalidArgument("Invalid spreader PCM block"));
        }
        for slot in 0..SLOTS {
            for channel in 0..count {
                self.input_hop[channel * HOP..(channel + 1) * HOP].copy_from_slice(
                    &input[channel * FRAME + slot * HOP..channel * FRAME + (slot + 1) * HOP],
                );
            }
            self.bank
                .analysis(&self.input_hop, &mut self.input_spectrum)?;
            for band in 0..BANDS {
                for channel in 0..count {
                    self.input_tf[(band * count + channel) * SLOTS + slot] =
                        self.input_spectrum[band * count + channel];
                }
            }
        }
        self.output_tf.fill(C::default());
        for (index, source) in self.sources.iter_mut().enumerate() {
            for band in 0..BANDS {
                for ear in 0..2 {
                    for slot in 0..SLOTS {
                        self.proto[(band * 2 + ear) * SLOTS + slot] = self.input_tf
                            [(band * count + index) * SLOTS + slot]
                            * source.mean[band][ear];
                    }
                }
            }
            source.decor.apply(&self.proto, &mut self.decor);
            for band in 0..BANDS {
                let active = self.frequencies[band] < 16000.;
                let (mix, residual) = if active {
                    let mut covariance = M::zeros();
                    let mut energy = 0.;
                    for slot in 0..SLOTS {
                        let value = Vector2::new(
                            self.proto[(band * 2) * SLOTS + slot],
                            self.proto[(band * 2 + 1) * SLOTS + slot],
                        );
                        covariance += value * value.adjoint();
                        energy += self.input_tf[(band * count + index) * SLOTS + slot].norm_sqr();
                    }
                    source.input_cov[band] =
                        source.input_cov[band] * C::new(0.9, 0.) + covariance * C::new(0.1, 0.);
                    let target =
                        source.shape[band] * C::new(energy * source.center_energy[band], 0.);
                    source.target_cov[band] =
                        source.target_cov[band] * C::new(0.9, 0.) + target * C::new(0.1, 0.);
                    let mut regularized = source.input_cov[band];
                    regularized[(0, 0)].re += 0.00001;
                    regularized[(1, 1)].re += 0.00001;
                    let (mix, remainder) =
                        mixing::complex_mix(regularized, source.target_cov[band], 0.2)?;
                    let diagonal = R::from_diagonal(&Vector2::new(
                        regularized[(0, 0)].re,
                        regularized[(1, 1)].re,
                    ));
                    (mix, mixing::real_mix(diagonal, remainder, 0.2)?)
                } else {
                    (M::identity(), R::zeros())
                };
                for slot in 0..SLOTS {
                    let alpha = (slot + 1) as f32 / SLOTS as f32;
                    let m =
                        mix * C::new(alpha, 0.) + source.previous[band] * C::new(1. - alpha, 0.);
                    let mr = residual * alpha + source.previous_residual[band] * (1. - alpha);
                    for ear in 0..2 {
                        let mut value = C::default();
                        for lane in 0..2 {
                            value += m[(ear, lane)] * self.proto[(band * 2 + lane) * SLOTS + slot];
                            if active {
                                value +=
                                    self.decor[(band * 2 + lane) * SLOTS + slot] * mr[(ear, lane)];
                            }
                        }
                        self.output_tf[(band * 2 + ear) * SLOTS + slot] += value;
                    }
                }
                source.previous[band] = mix;
                source.previous_residual[band] = residual;
            }
        }
        for slot in 0..SLOTS {
            for band in 0..BANDS {
                for ear in 0..2 {
                    self.output_spectrum[band * 2 + ear] =
                        self.output_tf[(band * 2 + ear) * SLOTS + slot];
                }
            }
            self.bank
                .synthesis(&self.output_spectrum, &mut self.output_hop)?;
            for ear in 0..2 {
                output[ear * FRAME + slot * HOP..ear * FRAME + (slot + 1) * HOP]
                    .copy_from_slice(&self.output_hop[ear * HOP..(ear + 1) * HOP]);
            }
        }
        if output.iter().any(|v| !v.is_finite()) {
            return Err(Error::RenderFailed("Nonfinite spreader output"));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn source_state_is_reproducible_independent_of_other_instances() {
        let dirs = [0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.];
        let mut ir = vec![0.; 6 * 2 * 16];
        for dir in 0..6 {
            ir[(dir * 2) * 16 + 2] = 0.7 + dir as f32 * 0.02;
            ir[(dir * 2 + 1) * 16 + 4] = 0.8 - dir as f32 * 0.01;
        }
        let mut a = Spreader::new(&ir, &dirs, 16, 48000, &[42]).unwrap();
        let _other = Spreader::new(&ir, &dirs, 16, 48000, &[1234]).unwrap();
        let mut b = Spreader::new(&ir, &dirs, 16, 48000, &[42]).unwrap();
        a.set_source(0, 30., 15., 150.).unwrap();
        b.set_source(0, 30., 15., 150.).unwrap();
        let mut x = [0.; FRAME];
        let mut y = [0.; FRAME * 2];
        let mut z = [0.; FRAME * 2];
        let mut energy = 0.;
        for block in 0..20 {
            for (i, value) in x.iter_mut().enumerate() {
                *value = ((i + block * FRAME) * 77 % 251) as f32 / 251. - 0.5;
            }
            a.process(&x, &mut y).unwrap();
            b.process(&x, &mut z).unwrap();
            assert!(y.iter().zip(&z).all(|(a, b)| a.to_bits() == b.to_bits()));
            energy += y.iter().map(|v| v * v).sum::<f32>();
        }
        assert!(energy > 1.);
        assert!(a.set_source(1, 0., 0., 0.).is_err());
    }
}
