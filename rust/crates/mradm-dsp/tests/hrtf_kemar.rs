//! HRTF geometry and lookup over the built-in KEMAR set and a partial sphere (formerly the C++
//! `hrtf_grid_test` and `hrtf_interpolation_test`, R3 B). The interpolation oracle below is an
//! independent statement of the lookup contract — bilinear cells over the 1° grid and a
//! magnitude-preserving three-direction blend — written against the bank's spectrum snapshot, so it
//! checks coordinate handling, blending and the bin / ear layout of both bank layouts.
use mradm_dsp::dataset::Dataset;
use mradm_dsp::hrtf::Grid;
use mradm_dsp::hrtf_filters::{Filters, Lookup};
use std::sync::Arc;

const PARTIAL: [f32; 16] = [
    -135., 0., -45., 0., 45., 0., 135., 0., 0., 45., 90., 45., 180., 45., -90., 45.,
];

fn check_grid(directions: &[f32]) {
    let grid = Grid::new(directions).unwrap();
    let repeat = Grid::new(directions).unwrap();
    assert_eq!(
        grid.weights(),
        repeat.weights(),
        "geometry is deterministic"
    );
    assert_eq!(grid.indices(), repeat.indices());
    assert_eq!(grid.weights().len(), 361 * 181 * 3);
    assert_eq!(grid.indices().len(), grid.weights().len());
    let count = directions.len() / 2;
    for (weights, indices) in grid
        .weights()
        .as_chunks::<3>()
        .0
        .iter()
        .zip(grid.indices().as_chunks::<3>().0.iter())
    {
        assert!(weights.iter().all(|w| w.is_finite() && *w >= 0.));
        assert!(indices.iter().all(|&i| i >= 0 && (i as usize) < count));
        let sum: f32 = weights.iter().sum();
        assert!(sum == 0. || (sum - 1.).abs() < 1e-6, "{sum}");
    }
}

#[test]
fn grids_are_normalized_bounded_and_deterministic() {
    check_grid(&Dataset::kemar().directions);
    check_grid(&PARTIAL);
    let mut nonfinite = PARTIAL;
    nonfinite[0] = f32::NAN;
    assert!(Grid::new(&nonfinite).is_err());
}

#[derive(Clone, Copy, Default)]
struct C(f32, f32);
impl C {
    fn abs(self) -> f32 {
        self.0.hypot(self.1)
    }
}

/// The lookup contract, independently of the bank's storage.
struct Oracle {
    bins: usize,
    directions: usize,
    weights: Vec<f32>,
    indices: Vec<i32>,
    spectra: Vec<C>,
}

impl Oracle {
    fn new(f: &Filters) -> Self {
        let mut raw = vec![0f32; f.spectrum_len()];
        f.copy_spectra(&mut raw).unwrap();
        let directions = f.grid().direction_count();
        Self {
            bins: f.output_len() / 4,
            directions,
            weights: f.grid().weights().to_vec(),
            indices: f.grid().indices().to_vec(),
            spectra: raw
                .as_chunks::<2>()
                .0
                .iter()
                .map(|p| C(p[0], p[1]))
                .collect(),
        }
    }

    fn grid_bin(&self, grid: usize, bin: usize, ear: usize) -> C {
        let (mut magnitude, mut sum) = (0f32, C::default());
        for k in 0..3 {
            let gain = self.weights[grid * 3 + k];
            let direction = self.indices[grid * 3 + k] as usize;
            let h = self.spectra[(bin * 2 + ear) * self.directions + direction];
            magnitude += gain * h.abs();
            sum = C(sum.0 + gain * h.0, sum.1 + gain * h.1);
        }
        let complex = sum.abs();
        if complex > 1e-9 {
            let s = magnitude / complex;
            C(sum.0 * s, sum.1 * s)
        } else {
            C(magnitude, 0.)
        }
    }

    fn query(&self, azimuth: f32, elevation: f32, continuous: bool) -> Vec<C> {
        let mut az = (azimuth + 180.) % 360.;
        if az < 0. {
            az += 360.;
        }
        let el = (elevation + 90.).clamp(0., 180.);
        let mut out = vec![C::default(); self.bins * 2];
        if !continuous {
            let (x, y) = ((az.round() as usize).min(360), el.round() as usize);
            for bin in 0..self.bins {
                for ear in 0..2 {
                    out[bin * 2 + ear] = self.grid_bin(y * 361 + x, bin, ear);
                }
            }
            return out;
        }
        let (x0, y0) = (az.floor() as usize, el.floor() as usize);
        let (x1, y1) = ((x0 + 1) % 360, (y0 + 1).min(180));
        let (a, e) = (az - x0 as f32, el - y0 as f32);
        let corners = [y0 * 361 + x0, y0 * 361 + x1, y1 * 361 + x0, y1 * 361 + x1];
        let weights = [(1. - a) * (1. - e), a * (1. - e), (1. - a) * e, a * e];
        for (corner, weight) in corners.into_iter().zip(weights) {
            if weight == 0. {
                continue;
            }
            for bin in 0..self.bins {
                for ear in 0..2 {
                    let h = self.grid_bin(corner, bin, ear);
                    let o = &mut out[bin * 2 + ear];
                    *o = C(o.0 + weight * h.0, o.1 + weight * h.1);
                }
            }
        }
        out
    }
}

fn points() -> Vec<(f32, f32)> {
    let mut points = Vec::new();
    for az in [
        -540., -180.00001, -180., -179.99999, -179.5, -0.51, -0.5, 0., 0.49, 0.5, 179.49, 179.5,
        179.99999, 180., 540.,
    ] {
        for el in [-200., -90., -89.999, -0.5, 0., 0.5, 89.999, 90., 200.] {
            points.push((az, el));
        }
    }
    for i in 0..512usize {
        points.push((
            ((i * 7919) % 72000) as f32 * 0.01 - 360.,
            ((i * 3571) % 24000) as f32 * 0.01 - 120.,
        ));
    }
    points
}

fn verify(directions: &[f32], impulses: &[f32], taps: usize, fft: usize) {
    let grid = Arc::new(Grid::new(directions).unwrap());
    let offline = Filters::new(grid.clone(), impulses, taps, fft, false).unwrap();
    let live = Filters::new(grid, impulses, taps, fft, true).unwrap();
    // Only the live layout keeps a magnitude per stored spectrum value.
    assert_eq!(
        live.storage_bytes() - offline.storage_bytes(),
        offline.spectrum_len() / 2 * size_of::<f32>()
    );
    let oracle = Oracle::new(&offline);
    for (az, el) in points() {
        for continuous in [false, true] {
            let expected = oracle.query(az, el, continuous);
            let lookup = if continuous {
                Lookup::Continuous
            } else {
                Lookup::Quantized
            };
            for bank in [&offline, &live] {
                let mut actual = vec![0f32; bank.output_len()];
                bank.query(az, el, lookup, &mut actual, None).unwrap();
                for (a, e) in actual.as_chunks::<2>().0.iter().zip(&expected) {
                    let error = C(a[0] - e.0, a[1] - e.1).abs();
                    assert!(
                        error <= 2e-6 + 2e-6 * e.abs(),
                        "az={az} el={el} continuous={continuous}: {error}"
                    );
                }
            }
        }
    }
}

#[test]
fn kemar_lookups_match_the_contract_in_both_layouts() {
    let kemar = Dataset::kemar();
    verify(&kemar.directions, &kemar.impulses, kemar.ir_len, 512);
}

#[test]
fn partial_sphere_lookups_match_the_contract_in_both_layouts() {
    let mut impulses = vec![0f32; 8 * 2 * 16];
    for direction in 0..8 {
        impulses[direction * 32 + direction] = (direction + 1) as f32 * 0.125;
        impulses[direction * 32 + 16 + direction] = -0.25;
    }
    verify(&PARTIAL, &impulses, 16, 32);
}

#[test]
fn malformed_banks_are_rejected() {
    let kemar = Dataset::kemar();
    let grid = Arc::new(Grid::new(&kemar.directions).unwrap());
    assert!(Filters::new(grid.clone(), &[], kemar.ir_len, 512, false).is_err());
    let short = &kemar.impulses[..kemar.impulses.len() - 1];
    assert!(Filters::new(grid.clone(), short, kemar.ir_len, 512, false).is_err());
    assert!(Filters::new(grid, &kemar.impulses, kemar.ir_len, usize::MAX, false).is_err());
}
