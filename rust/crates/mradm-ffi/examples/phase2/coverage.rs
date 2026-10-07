//! Follow-up probes exercise production implementations with exact binary inputs.
use super::{floats, integers, signal};
use mradm_dsp::{
    Complex32 as C,
    hrtf::Grid,
    hrtf_filters::{Filters, Lookup, Trace},
    mixing::{self, ComplexMatrix},
};
use std::{path::Path, sync::Arc};

struct CovarianceCase {
    name: &'static str,
    x: ComplexMatrix,
    y: ComplexMatrix,
    regularization: f32,
}

fn covariance(a: f32, d: f32, re: f32, im: f32) -> ComplexMatrix {
    ComplexMatrix::new(
        C::new(a, 0.),
        C::new(re, im),
        C::new(re, -im),
        C::new(d, 0.),
    )
}

fn covariance_cases() -> [CovarianceCase; 8] {
    let target = covariance(0.5, 0.25, -0.125, 0.);
    let floor = 2.23e-13_f32;
    let tiny = f32::from_bits((127 - 60) << 23);
    let epsilon = f32::from_bits((127 - 22) << 23);
    [
        CovarianceCase {
            name: "zero",
            x: ComplexMatrix::zeros(),
            y: ComplexMatrix::zeros(),
            regularization: 0.125,
        },
        CovarianceCase {
            name: "repeated",
            x: covariance(0.5, 0.5, 0., 0.),
            y: covariance(0.25, 0.25, 0., 0.),
            regularization: 0.,
        },
        CovarianceCase {
            name: "rank-one",
            x: covariance(0.5, 0.5, 0.5, 0.),
            y: target,
            regularization: 0.125,
        },
        CovarianceCase {
            name: "antiphase",
            x: covariance(0.5, 0.5, -0.5, 0.),
            y: target,
            regularization: 0.125,
        },
        CovarianceCase {
            name: "near-rank-one",
            x: covariance(0.5, 0.5, 0.5 - epsilon, epsilon * 0.25),
            y: target,
            regularization: 0.125,
        },
        CovarianceCase {
            name: "tiny",
            x: covariance(tiny, tiny, 0., 0.),
            y: covariance(tiny * 0.25, tiny * 0.25, 0., 0.),
            regularization: 0.125,
        },
        CovarianceCase {
            name: "complex-rank-one",
            x: covariance(0.5, 0.5, 0., 0.5),
            y: covariance(0.5, 0.25, -0.25, 0.125),
            regularization: 0.125,
        },
        CovarianceCase {
            name: "floor-adjacent",
            x: covariance(
                f32::from_bits(floor.to_bits() - 1),
                f32::from_bits(floor.to_bits() + 1),
                0.,
                0.,
            ),
            y: target,
            regularization: 0.125,
        },
    ]
}

fn covariance_probes(out: &Path) {
    let cases = covariance_cases();
    verify_covariance_fixtures(&cases);
    for case in cases {
        let prefix = format!("om-edge-{}", case.name);
        mradm_dsp::diagnostics::scope(&prefix);
        let mut input: Vec<_> = case
            .x
            .iter()
            .chain(case.y.iter())
            .flat_map(|v| [v.re, v.im])
            .collect();
        input.push(case.regularization);
        floats(out, &format!("{prefix}.10-input.f32"), &input);
        let real = mixing::real_mix(
            case.x.map(|v| v.re),
            case.y.map(|v| v.re),
            case.regularization,
        )
        .unwrap();
        let (complex, residual) = mixing::complex_mix(case.x, case.y, case.regularization).unwrap();
        floats(out, &format!("{prefix}.20-real.f32"), real.as_slice());
        floats(
            out,
            &format!("{prefix}.30-complex.c32"),
            &complex
                .iter()
                .flat_map(|v| [v.re, v.im])
                .collect::<Vec<_>>(),
        );
        floats(
            out,
            &format!("{prefix}.40-residual.f32"),
            residual.as_slice(),
        );
    }
}

fn same_bits(a: &[f32], b: &[f32]) {
    assert_eq!(a.len(), b.len());
    assert!(a.iter().zip(b).all(|(x, y)| x.to_bits() == y.to_bits()));
}

fn hrtf_probes(out: &Path, size: usize) {
    let prefix = format!("hrtf-{size}");
    mradm_dsp::diagnostics::scope(&prefix);
    let mut directions = vec![0., 90., 0., -90.];
    for (ring, elevation) in [-45_f32, 0., 30., 60.].into_iter().enumerate() {
        for azimuth in 0..6 {
            directions.extend([azimuth as f32 * 60. + ring as f32 * 7.5 - 180., elevation]);
        }
    }
    let taps = 64;
    let mut impulses = signal(directions.len() * taps);
    for (i, value) in impulses.iter_mut().enumerate() {
        // Exact powers of two; no platform math is used to generate the HRIR.
        *value *= f32::from_bits((127 - (i % taps / 8) as u32) << 23);
    }
    let queries = [
        [-180., 0.],
        [180., 0.],
        [-179.75, 0.25],
        [179.75, -0.25],
        [0., 90.],
        [0., -90.],
        [37.125, 89.875],
        [-101.25, -89.875],
        [37.125, 23.25],
        [-19.5, -0.125],
        [540., 12.25],
        [-540., -12.25],
    ];
    floats(out, &format!("{prefix}.10-directions.f32"), &directions);
    floats(out, &format!("{prefix}.11-hrir.f32"), &impulses);
    floats(
        out,
        &format!("{prefix}.12-queries.f32"),
        queries.as_flattened(),
    );
    integers(
        out,
        &format!("{prefix}.13-parameters.i32"),
        &[taps as i32, size as i32, 26],
    );
    let grid = Arc::new(Grid::new(&directions).unwrap());
    floats(
        out,
        &format!("{prefix}.20-grid-weights.f32"),
        grid.weights(),
    );
    integers(
        out,
        &format!("{prefix}.21-grid-indices.i32"),
        grid.indices(),
    );
    let filters = Filters::new(Arc::clone(&grid), &impulses, taps, size, false).unwrap();
    let cached = Filters::new(grid, &impulses, taps, size, true).unwrap();
    let mut spectra = vec![0.; filters.spectrum_len()];
    filters.copy_spectra(&mut spectra).unwrap();
    floats(out, &format!("{prefix}.30-spectra.c32"), &spectra);
    let mut output = vec![0.; filters.output_len()];
    let mut reference = output.clone();
    let mut magnitudes = vec![0.; output.len() / 2 * 3];
    let mut sum = output.clone();
    let mut scales = output.clone();
    let (mut quantized, mut continuous, mut all_magnitudes, mut all_sum, mut all_scales) =
        (Vec::new(), Vec::new(), Vec::new(), Vec::new(), Vec::new());
    for [azimuth, elevation] in queries {
        filters
            .query(
                azimuth,
                elevation,
                Lookup::Quantized,
                &mut output,
                Some(Trace {
                    magnitudes: &mut magnitudes,
                    complex_sum: &mut sum,
                    scales: &mut scales,
                }),
            )
            .unwrap();
        cached
            .query(azimuth, elevation, Lookup::Quantized, &mut reference, None)
            .unwrap();
        same_bits(&output, &reference);
        quantized.extend_from_slice(&output);
        all_magnitudes.extend_from_slice(&magnitudes);
        all_sum.extend_from_slice(&sum);
        all_scales.extend_from_slice(&scales);
        filters
            .query(azimuth, elevation, Lookup::Continuous, &mut output, None)
            .unwrap();
        cached
            .query(azimuth, elevation, Lookup::Continuous, &mut reference, None)
            .unwrap();
        same_bits(&output, &reference);
        continuous.extend_from_slice(&output);
    }
    floats(out, &format!("{prefix}.40-quantized.c32"), &quantized);
    floats(out, &format!("{prefix}.41-magnitudes.f32"), &all_magnitudes);
    floats(out, &format!("{prefix}.42-complex-sum.c32"), &all_sum);
    floats(out, &format!("{prefix}.43-scales.f32"), &all_scales);
    floats(out, &format!("{prefix}.50-continuous.c32"), &continuous);
}

pub(super) fn run(out: &Path) {
    covariance_probes(out);
    for size in [256, 1024] {
        hrtf_probes(out, size);
    }
    mradm_dsp::diagnostics::scope("");
}

// Evidence generation itself checks these labels, so every native Release run
// rejects a fixture that silently ceased to be singular or near-singular.
fn verify_covariance_fixtures(cases: &[CovarianceCase]) {
    for case in cases {
        for matrix in [case.x, case.y] {
            assert_eq!(matrix, matrix.adjoint());
            let (a, d) = (f64::from(matrix[(0, 0)].re), f64::from(matrix[(1, 1)].re));
            let off = matrix[(0, 1)];
            let determinant = a * d
                - f64::from(off.re) * f64::from(off.re)
                - f64::from(off.im) * f64::from(off.im);
            assert!(a >= 0. && d >= 0. && determinant >= 0.);
        }
    }
    for name in ["zero", "rank-one", "antiphase", "complex-rank-one"] {
        let x = cases.iter().find(|case| case.name == name).unwrap().x;
        assert_eq!(x.determinant(), C::new(0., 0.));
    }
    let repeated = cases.iter().find(|case| case.name == "repeated").unwrap().x;
    assert_eq!(repeated, ComplexMatrix::identity() * C::new(0.5, 0.));
    let near = cases
        .iter()
        .find(|case| case.name == "near-rank-one")
        .unwrap()
        .x;
    assert!(near.determinant().re > 0. && near.determinant().re < 1e-6);
}
