use mradm_dsp::{
    hrtf::Grid,
    hrtf_filters::{Filters, Lookup, Trace, grid_index},
};
use std::sync::{Arc, OnceLock};

const DIRECTIONS: [f32; 12] = [0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.];
fn grid() -> Arc<Grid> {
    static GRID: OnceLock<Arc<Grid>> = OnceLock::new();
    Arc::clone(GRID.get_or_init(|| Arc::new(Grid::new(&DIRECTIONS).unwrap())))
}
fn impulses() -> Vec<f32> {
    let mut ir = vec![0.0; 6 * 2 * 16];
    for d in 0..6 {
        for ear in 0..2 {
            let start = (d * 2 + ear) * 16;
            ir[start + d + ear] = (d + 1) as f32 * 0.125;
            ir[start + 15] = -0.07 * (ear + 1) as f32;
        }
    }
    ir
}
fn query(filters: &Filters, az: f32, el: f32, mode: Lookup) -> Vec<f32> {
    let mut out = vec![0.0; filters.output_len()];
    filters.query(az, el, mode, &mut out, None).unwrap();
    out
}
fn close(a: &[f32], b: &[f32], tolerance: f32) {
    assert_eq!(a.len(), b.len());
    for (&a, &b) in a.iter().zip(b) {
        assert!((a - b).abs() <= tolerance, "{a} != {b}");
    }
}

#[test]
fn spectra_and_measured_directions_match_independent_double_dft() {
    let ir = impulses();
    for fft_len in [16, 32, 128] {
        let filters = Filters::new(grid(), &ir, 16, fft_len, false).unwrap();
        let mut spectra = vec![0.0; filters.spectrum_len()];
        filters.copy_spectra(&mut spectra).unwrap();
        for d in 0..6 {
            let measured = query(
                &filters,
                DIRECTIONS[d * 2],
                DIRECTIONS[d * 2 + 1],
                Lookup::Quantized,
            );
            for ear in 0..2 {
                for bin in 0..=fft_len / 2 {
                    let (mut re, mut im) = (0.0f64, 0.0f64);
                    for tap in 0..16 {
                        let phase =
                            -std::f64::consts::TAU * bin as f64 * tap as f64 / fft_len as f64;
                        let value = ir[(d * 2 + ear) * 16 + tap] as f64;
                        re += value * phase.cos();
                        im += value * phase.sin();
                    }
                    let index = ((bin * 2 + ear) * 6 + d) * 2;
                    close(&spectra[index..index + 2], &[re as f32, im as f32], 2e-6);
                    let index = (bin * 2 + ear) * 2;
                    close(&measured[index..index + 2], &[re as f32, im as f32], 2e-6);
                }
            }
        }
    }
}

#[test]
fn cache_and_shared_concurrent_queries_are_identical() {
    let ir = impulses();
    let uncached = Filters::new(grid(), &ir, 16, 64, false).unwrap();
    let cached = Filters::new(grid(), &ir, 16, 64, true).unwrap();
    assert_eq!(
        cached.storage_bytes() - uncached.storage_bytes(),
        cached.spectrum_len() / 2 * size_of::<f32>()
    );
    for mode in [Lookup::Quantized, Lookup::Continuous] {
        for (az, el) in [
            (-179.8, -20.2),
            (0.49, 0.0),
            (33.25, 22.75),
            (179.99, 89.9),
            (520.0, -200.0),
        ] {
            let expected = query(&uncached, az, el, mode);
            assert_eq!(expected, query(&cached, az, el, mode));
            std::thread::scope(|scope| {
                let workers: Vec<_> = (0..4)
                    .map(|_| scope.spawn(|| query(&cached, az, el, mode)))
                    .collect();
                for worker in workers {
                    assert_eq!(expected, worker.join().unwrap());
                }
            });
        }
    }
}

#[test]
fn lookup_rounding_clamping_and_seams_retain_their_distinct_contracts() {
    let row = 90 * 361;
    for (az, expected) in [
        (-180.0, 0),
        (180.0, 0),
        (540.0, 0),
        (-540.0, 0),
        (179.49, 359),
        (179.5, 360),
        (-179.5, 1),
        (0.49, 180),
        (0.5, 181),
    ] {
        assert_eq!(grid_index(az, 0.0).unwrap(), row + expected);
    }
    assert_eq!(grid_index(0.0, -1e30).unwrap(), 180);
    assert_eq!(grid_index(0.0, 1e30).unwrap(), 180 * 361 + 180);
    let f = Filters::new(grid(), &impulses(), 16, 64, true).unwrap();
    for el in [-90.0, -20.0, 0.0, 22.0, 90.0] {
        assert_eq!(
            query(&f, -180.0, el, Lookup::Continuous),
            query(&f, 180.0, el, Lookup::Continuous)
        );
        assert_eq!(
            query(&f, 37.0, el, Lookup::Continuous),
            query(&f, 37.0, el, Lookup::Quantized)
        );
        let seam = query(&f, -180.0, el, Lookup::Continuous);
        for side in [-1.0, 1.0] {
            let near = query(&f, -180.0 + side * 0.0001, el, Lookup::Continuous);
            let far = query(&f, -180.0 + side * 0.001, el, Lookup::Continuous);
            for ((near, far), center) in near.into_iter().zip(far).zip(&seam) {
                assert!(
                    (near - center).abs() <= 0.2 * (far - center).abs() + 2e-7,
                    "seam approach must converge linearly"
                );
            }
        }
    }
    for (az, el) in [(0.5, 0.5), (21.5, -11.5), (179.5, 20.5)] {
        let midpoint = query(&f, az, el, Lookup::Continuous);
        let corners: Vec<_> = [(-0.5, -0.5), (0.5, -0.5), (-0.5, 0.5), (0.5, 0.5)]
            .into_iter()
            .map(|(a, e)| query(&f, az + a, el + e, Lookup::Quantized))
            .collect();
        for (index, sample) in midpoint.into_iter().enumerate() {
            let reference = corners.iter().map(|c| c[index] as f64).sum::<f64>() * 0.25;
            assert!((sample as f64 - reference).abs() < 1e-6);
        }
    }
}

#[test]
fn phase_cancellation_uses_positive_real_magnitude_and_trace_matches_kernel() {
    let grid = grid();
    let g = grid_index(45.0, 0.0).unwrap();
    let active: Vec<_> = (0..3)
        .filter(|&k| grid.weights()[g * 3 + k] > 0.0)
        .collect();
    assert_eq!(active.len(), 2);
    let (a, b) = (active[0], active[1]);
    let mut ir = vec![0.0; 12];
    ir[grid.indices()[g * 3 + a] as usize * 2] = grid.weights()[g * 3 + b];
    ir[grid.indices()[g * 3 + b] as usize * 2] = -grid.weights()[g * 3 + a];
    let expected = 2.0 * grid.weights()[g * 3 + a] * grid.weights()[g * 3 + b];
    for cached in [false, true] {
        let f = Filters::new(Arc::clone(&grid), &ir, 1, 2, cached).unwrap();
        let mut out = [0.0; 8];
        let (mut magnitudes, mut sum, mut scales) = ([0.0; 12], [0.0; 8], [0.0; 8]);
        f.query(
            45.,
            0.,
            Lookup::Quantized,
            &mut out,
            Some(Trace {
                magnitudes: &mut magnitudes,
                complex_sum: &mut sum,
                scales: &mut scales,
            }),
        )
        .unwrap();
        assert_eq!(out, [expected, 0., 0., 0., expected, 0., 0., 0.]);
        assert_eq!(sum, [0.0; 8]);
        assert_eq!(scales, out);
        for bin in 0..2 {
            for ear in 0..2 {
                for k in 0..3 {
                    assert_eq!(
                        magnitudes[(bin * 2 + ear) * 3 + k],
                        ir[grid.indices()[g * 3 + k] as usize * 2 + ear].abs()
                    );
                }
            }
        }
    }
}

#[test]
fn phase_fallback_threshold_preserves_tiny_delayed_responses() {
    for amplitude in [1e-10, 1e-8] {
        let mut ir = [0.0; 24];
        for pair in ir.as_chunks_mut::<2>().0 {
            pair[1] = amplitude;
        }
        let f = Filters::new(grid(), &ir, 2, 4, true).unwrap();
        let out = query(&f, 0., 0., Lookup::Quantized);
        // Bin 1 of a one-sample delay is purely -j. Below the existing threshold
        // its phase is deliberately replaced by zero while retaining magnitude.
        if amplitude < 1e-9 {
            close(&out[4..8], &[amplitude, 0., amplitude, 0.], 1e-15);
        } else {
            close(&out[4..8], &[0., -amplitude, 0., -amplitude], 1e-15);
        }
    }
}

#[test]
fn invalid_input_and_queries_fail_before_modifying_output() {
    let ir = impulses();
    for (taps, n, input) in [
        (0, 64, &ir[..]),
        (17, 64, &ir[..]),
        (16, 8, &ir[..]),
        (16, 63, &ir[..]),
        (usize::MAX, usize::MAX, &ir[..]),
        (16, 64, &ir[..ir.len() - 1]),
    ] {
        assert!(Filters::new(grid(), input, taps, n, false).is_err());
    }
    let mut bad = ir.clone();
    bad[0] = f32::NAN;
    assert!(Filters::new(grid(), &bad, 16, 64, false).is_err());
    bad.fill(f32::MAX);
    assert!(Filters::new(grid(), &bad, 16, 64, false).is_err());
    let f = Filters::new(grid(), &ir, 16, 64, true).unwrap();
    let mut out = vec![42.; f.output_len()];
    for (az, el) in [
        (f32::NAN, 0.0),
        (0.0, f32::INFINITY),
        (f32::NEG_INFINITY, 0.0),
    ] {
        for mode in [Lookup::Quantized, Lookup::Continuous] {
            assert!(f.query(az, el, mode, &mut out, None).is_err());
        }
    }
    assert!(
        f.query(0., 0., Lookup::Quantized, &mut out[..1], None)
            .is_err()
    );
    assert!(
        f.query(
            0.,
            0.,
            Lookup::Quantized,
            &mut out,
            Some(Trace {
                magnitudes: &mut [],
                complex_sum: &mut [],
                scales: &mut []
            })
        )
        .is_err()
    );
    assert!(out.iter().all(|&x| x == 42.));
    assert!(
        query(&f, 20., 10., Lookup::Continuous)
            .iter()
            .all(|x| x.is_finite())
    );
}

#[test]
fn geometry_outlives_creator_and_empty_sparse_rows_remain_silent() {
    let dirs = [
        -135., 0., -45., 0., 45., 0., 135., 0., 0., 45., 90., 45., 180., 45., -90., 45.,
    ];
    let grid = Arc::new(Grid::new(&dirs).unwrap());
    let ir = vec![0.5; 16];
    let f = Filters::new(Arc::clone(&grid), &ir, 1, 2, false).unwrap();
    drop(grid);
    for (g, weights) in f
        .grid()
        .weights()
        .as_chunks::<3>()
        .0
        .iter()
        .enumerate()
        .step_by(13)
    {
        let expected = if weights.iter().sum::<f32>() == 0. {
            0.
        } else {
            0.5
        };
        let out = query(
            &f,
            (g % 361) as f32 - 180.,
            (g / 361) as f32 - 90.,
            Lookup::Quantized,
        );
        close(
            &out,
            &[expected, 0., expected, 0., expected, 0., expected, 0.],
            1e-6,
        );
    }
}
