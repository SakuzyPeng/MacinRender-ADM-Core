//! VBAP and nine-direction MDAP, including the existing +/-90 degree dummy
//! loudspeakers. Only geometry preparation allocates; gain queries do not.
use crate::{Error, Result, geometry::*};

pub struct Panner {
    pub real_speakers: usize,
    pub vertices: Vec<Vec3>,
    pub triangles: Vec<Triplet>,
    pairs: Vec<([usize; 2], [[f64; 2]; 2])>,
    is_3d: bool,
}

impl Panner {
    pub fn new(dirs: &[f32], is_3d: bool) -> Result<Self> {
        let mut vertices = directions(dirs)?;
        let real_speakers = vertices.len();
        let mut triangles = Vec::new();
        let mut pairs = Vec::new();
        if is_3d {
            if !dirs.as_chunks::<2>().0.iter().any(|d| d[1] <= -60.0) {
                vertices.push([0.0, 0.0, -1.0]);
            }
            if !dirs.as_chunks::<2>().0.iter().any(|d| d[1] >= 60.0) {
                vertices.push([0.0, 0.0, 1.0]);
            }
            triangles = triplets(&vertices, true)?;
        } else {
            let mut order: Vec<usize> = (0..real_speakers).collect();
            order.sort_by(|&a, &b| dirs[2 * a].total_cmp(&dirs[2 * b]));
            for i in 0..order.len() {
                let ids = [order[i], order[(i + 1) % order.len()]];
                let a = vertices[ids[0]];
                let b = vertices[ids[1]];
                let det = a[0] * b[1] - a[1] * b[0];
                if det.abs() > 1e-12 {
                    pairs.push((ids, [[b[1] / det, -b[0] / det], [-a[1] / det, a[0] / det]]));
                }
            }
            if pairs.is_empty() {
                return Err(Error::InvalidArgument("Degenerate 2D speaker geometry"));
            }
        }
        Ok(Self {
            real_speakers,
            vertices,
            triangles,
            pairs,
            is_3d,
        })
    }

    pub fn gains(&self, az: f32, el: f32, spread: f32, output: &mut [f32]) -> Result<()> {
        if output.len() != self.real_speakers
            || !az.is_finite()
            || !el.is_finite()
            || !spread.is_finite()
        {
            return Err(Error::InvalidArgument("Invalid VBAP query"));
        }
        output.fill(0.0);
        let mut dummy = [0.0f32; 2];
        // Planar layouts discard height. Keep a unit horizontal direction even
        // at the poles so the pair-selection tolerance is independent of elevation.
        let center = direction(az, if self.is_3d { el } else { 0. });
        if !self.is_3d {
            for (ids, inverse) in &self.pairs {
                let g = inverse.map(|row| row[0] * center[0] + row[1] * center[1]);
                let length = (g[0] * g[0] + g[1] * g[1]).sqrt();
                if g[0].min(g[1]) > -0.001 && length > 0.0 {
                    for i in 0..2 {
                        output[ids[i]] = (g[i] / length) as f32;
                    }
                }
            }
        } else {
            let mut accumulate = |query: Vec3, all: bool| {
                for t in &self.triangles {
                    let gains = t.inverse.map(|row| dot(row, query));
                    let length = norm(gains);
                    if gains.iter().all(|&g| g > -0.001) && length > 0.0 {
                        for (i, raw_gain) in gains.iter().enumerate() {
                            let id = t.vertices[i];
                            let gain = (raw_gain / length) as f32;
                            if id < self.real_speakers {
                                output[id] += gain;
                            } else {
                                dummy[id - self.real_speakers] += gain;
                            }
                        }
                        if !all {
                            break;
                        }
                    }
                }
            };
            if spread > 0.1 {
                let base = if center[2].abs() > 0.9999 {
                    [1.0, 0.0, 0.0]
                } else {
                    unit(cross(center, [0.0, 0.0, 1.0]))
                };
                let tangent = unit(cross(center, base));
                let (sine, cosine) = ((spread.clamp(0.0, 180.0) as f64) * 0.5)
                    .to_radians()
                    .sin_cos();
                for i in 0..8 {
                    let (s, c) = (i as f64 * std::f64::consts::TAU / 8.0).sin_cos();
                    accumulate(
                        add(
                            scale(center, cosine),
                            scale(add(scale(base, c), scale(tangent, s)), sine),
                        ),
                        true,
                    );
                }
                accumulate(center, true);
            } else {
                accumulate(center, false);
            }
        }
        let energy: f64 = output
            .iter()
            .chain(dummy.iter())
            .map(|&g| (g as f64).powi(2))
            .sum();
        if energy > 0.0 {
            let inverse = 1.0 / energy.sqrt();
            for value in output {
                *value = ((*value as f64) * inverse).max(0.0) as f32;
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn stereo_and_surround_power_and_routing() {
        let p = Panner::new(&[30., 0., -30., 0.], false).unwrap();
        let mut g = [0.; 2];
        p.gains(0., 0., 0., &mut g).unwrap();
        assert!((g[0] - std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-6);
        assert_eq!(g[0], g[1]);
        p.gains(30., 0., 0., &mut g).unwrap();
        assert!((g[0] - 1.0).abs() < 1e-6 && g[1] < 1e-6);
        p.gains(180., 0., 0., &mut g).unwrap();
        assert_eq!(g, [0., 0.]);
        let p = Panner::new(&[30., 0., -30., 0., 0., 0., 110., 0., -110., 0.], false).unwrap();
        let mut g = [0.; 5];
        for angle in -180..=180 {
            p.gains(angle as f32, 0., 0., &mut g).unwrap();
            assert!((g.iter().map(|x| x * x).sum::<f32>() - 1.0).abs() < 1e-5);
        }
    }
    #[test]
    fn planar_layout_projects_elevated_sources_without_changing_gains() {
        let p = Panner::new(&[30., 0., -30., 0., 0., 0., 110., 0., -110., 0.], false).unwrap();
        let mut horizontal = [0.; 5];
        let mut elevated = [0.; 5];
        for azimuth in -180..=180 {
            p.gains(azimuth as f32, 0., 0., &mut horizontal).unwrap();
            for elevation in [-90., -89.99, -60., 60., 89.99, 90.] {
                p.gains(azimuth as f32, elevation, 0., &mut elevated)
                    .unwrap();
                assert_eq!(
                    elevated, horizontal,
                    "azimuth={azimuth}, elevation={elevation}"
                );
                assert!((elevated.iter().map(|g| g * g).sum::<f32>() - 1.).abs() < 1e-5);
            }
        }
        p.gains(0., 90., 0., &mut elevated).unwrap();
        assert_eq!(elevated, [0., 0., 1., 0., 0.]);
    }
    #[test]
    fn three_dimensional_spread_is_finite_and_uses_height() {
        let dirs = [
            30., 0., -30., 0., 0., 0., 110., 0., -110., 0., 45., 45., -45., 45., 135., 45., -135.,
            45.,
        ];
        let p = Panner::new(&dirs, true).unwrap();
        let mut point = [0.; 9];
        let mut wide = [0.; 9];
        p.gains(0., 0., 0., &mut point).unwrap();
        p.gains(0., 0., 90., &mut wide).unwrap();
        assert!(wide[5..].iter().sum::<f32>() > point[5..].iter().sum::<f32>());
        for spread in [0., 0.1, 0.11, 45., 179.999, 180.] {
            for angle in (-180..=180).step_by(5) {
                p.gains(angle as f32, 20., spread, &mut wide).unwrap();
                assert!(wide.iter().all(|x| x.is_finite() && *x >= 0.));
                assert!(wide.iter().map(|x| x * x).sum::<f32>() <= 1.00001);
            }
        }
    }
}
