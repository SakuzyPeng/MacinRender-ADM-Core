use super::*;
use crate::hrtf::Grid;
use std::sync::OnceLock;

fn bank(fft: usize, scale: f32) -> Arc<Filters> {
    static GRID: OnceLock<Arc<Grid>> = OnceLock::new();
    let grid = Arc::clone(GRID.get_or_init(|| {
        Arc::new(Grid::new(&[0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.]).unwrap())
    }));
    let taps = fft.min(16);
    let mut ir = vec![0.; 12 * taps];
    for d in 0..12 {
        ir[d * taps + d % taps] = scale * (d as f32 - 5.5) * 0.125;
        ir[d * taps + taps - 1] += scale * 0.03125;
    }
    Arc::new(Filters::new(grid, &ir, taps, fft, true).unwrap())
}

fn equal_bits(a: &[f32], b: &[f32]) {
    assert_eq!(a.len(), b.len());
    for (i, (a, b)) in a.iter().zip(b).enumerate() {
        assert_eq!(a.to_bits(), b.to_bits(), "sample {i}: {a} != {b}");
    }
}

#[test]
fn cached_queries_match_reference_through_seams_poles_eviction_and_clear() {
    for (fft, scale) in [(2, 1e-10), (64, 1e-8), (2048, 1.)] {
        let bank = bank(fft, scale);
        let mut expected = vec![0.; bank.output_len()];
        let mut actual = expected.clone();
        let edges = [
            [-180., -90.],
            [180., 90.],
            [540., 0.],
            [-540., 0.],
            [179.99998, 89.99998],
            [-179.99998, -89.99998],
            [0., -0.],
            [-0., 0.],
            [0.49, 0.5],
            [0.51, -0.5],
            [f32::MAX, f32::MAX],
            [-f32::MAX, -f32::MAX],
        ];
        for (corners, queries) in [(0, 0), (1, 1), (3, 2), (512, 0), (0, 128), (512, 128)] {
            let mut cache = CachedQueries::with_limits(Arc::clone(&bank), corners, queries);
            for step in 0..160 {
                let [az, el] = if step < edges.len() {
                    edges[step]
                } else {
                    [
                        step as f32 * 137.03125 - 900.,
                        step as f32 * 11.0625 - 1000.,
                    ]
                };
                bank.query(az, el, Lookup::Continuous, &mut expected, None)
                    .unwrap();
                for _ in 0..2 {
                    cache.query(az, el, &mut actual).unwrap();
                    equal_bits(&expected, &actual);
                }
                if step % 41 == 0 {
                    cache.clear();
                }
            }
        }
    }
}

#[test]
fn adjacent_directions_reuse_corners_and_exact_queries_use_normalized_bits() {
    let mut cache = CachedQueries::new(bank(64, 1.));
    let mut out = vec![0.; cache.bank.output_len()];
    cache.query(21.25, 30.25, &mut out).unwrap();
    assert_eq!(cache.corners.used, 4);
    cache.query(21.75, 30.75, &mut out).unwrap();
    assert_eq!(cache.corners.used, 4);
    assert_eq!(cache.queries.used, 2);
    let expected = out.clone();
    // Equivalent after the existing f32 normalization, without rounding a pose.
    cache.query(381.75, 30.75, &mut out).unwrap();
    equal_bits(&expected, &out);
    assert_eq!(cache.queries.used, 2);
    for (az, el) in [(f32::NAN, 0.), (0., f32::INFINITY)] {
        assert!(cache.query(az, el, &mut out).is_err());
        equal_bits(&expected, &out);
    }
    assert!(cache.query(21.75, 30.75, &mut out[..1]).is_err());
    equal_bits(&expected, &out);
    cache.clear();
    assert_eq!(cache.corners.used, 0);
    assert_eq!(cache.queries.used, 0);
    cache.query(21.75, 30.75, &mut out).unwrap();
    equal_bits(&expected, &out);
}

#[test]
fn lru_retains_recent_hits_and_budget_includes_values_keys_and_links() {
    let mut cache = Spectra::new(4, 3, 1024);
    for key in 0..3 {
        let slot = cache.insert(key).unwrap();
        cache.get_mut(slot).fill(key as f32);
    }
    assert_eq!(cache.find(0), Some(0));
    let slot = cache.insert(3).unwrap();
    assert_eq!(slot, 1);
    cache.get_mut(slot).fill(3.);
    assert_eq!(cache.find(1), None);
    assert_eq!(cache.get(cache.newest), &[3.; 4]);
    for width in [8, 4100, 131076, usize::MAX] {
        let corners = Spectra::<usize>::new(width, 512, CORNER_BYTES - size_of::<Arc<Filters>>());
        let queries = Spectra::<[u32; 2]>::new(width, 128, QUERY_BYTES);
        assert!(corners.entries.len() <= 512);
        assert!(queries.entries.len() <= 128);
        assert!(
            corners.storage_bytes() + queries.storage_bytes() + size_of::<Arc<Filters>>()
                <= 8 * 1024 * 1024
        );
    }
    let mut no_space = Spectra::<usize>::new(1024, 512, size_of::<Spectra<usize>>());
    assert!(no_space.insert(0).is_none());
}

#[test]
fn sessions_bind_caches_to_their_bank_and_can_query_concurrently() {
    let a = bank(64, 1.);
    let b = bank(64, 0.25);
    std::thread::scope(|s| {
        for bank in [&a, &b, &a, &b] {
            s.spawn(|| {
                let mut cache = CachedQueries::new(Arc::clone(bank));
                let mut actual = vec![0.; bank.output_len()];
                let mut expected = actual.clone();
                for az in 0..180 {
                    cache.query(az as f32 + 0.25, 22.5, &mut actual).unwrap();
                    bank.query(
                        az as f32 + 0.25,
                        22.5,
                        Lookup::Continuous,
                        &mut expected,
                        None,
                    )
                    .unwrap();
                    equal_bits(&expected, &actual);
                }
            });
        }
    });
}
