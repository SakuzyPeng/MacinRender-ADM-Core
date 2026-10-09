//! External SOFA bytes pass through the production reader, grid and filters.
use super::{floats, integers};
use mradm_dsp::{
    dataset::Dataset,
    hrtf::Grid,
    hrtf_filters::{Filters, Lookup},
};
use std::{fs, path::Path, sync::Arc};

pub(super) fn run(out: &Path, fixtures: &Path) {
    for (name, rate, directions, taps) in [
        ("simple", 48000, 6, 16),
        ("general-compressed", 44100, 6, 16),
        ("off-axis-compressed", 48000, 26, 64),
    ] {
        let prefix = format!("sofa-{name}");
        mradm_dsp::diagnostics::scope(&prefix);
        let bytes = fs::read(fixtures.join(format!("{name}.sofa"))).unwrap();
        let data = Dataset::sofa(&bytes).unwrap();
        assert_eq!(
            (data.sample_rate, data.num_dirs, data.ir_len),
            (rate, directions, taps)
        );
        integers(
            out,
            &format!("{prefix}.10-parameters.i32"),
            &[rate as i32, directions as i32, taps as i32, 1024],
        );
        floats(
            out,
            &format!("{prefix}.11-directions.f32"),
            &data.directions,
        );
        floats(out, &format!("{prefix}.12-hrir.f32"), &data.impulses);
        let grid = Arc::new(Grid::new(&data.directions).unwrap());
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
        let filters = Filters::new(grid, &data.impulses, taps, 1024, true).unwrap();
        let mut spectra = vec![0.; filters.spectrum_len()];
        filters.copy_spectra(&mut spectra).unwrap();
        floats(out, &format!("{prefix}.30-spectra.c32"), &spectra);
        let queries = [
            [0., 0.],
            [37.125, 23.25],
            [-179.75, 0.125],
            [179.75, -0.125],
            [-101.25, -89.875],
            [37.125, 89.875],
        ];
        floats(
            out,
            &format!("{prefix}.40-queries.f32"),
            queries.as_flattened(),
        );
        for (mode, suffix) in [
            (Lookup::Quantized, "50-quantized"),
            (Lookup::Continuous, "60-continuous"),
        ] {
            let mut all = Vec::new();
            let mut values = vec![0.; filters.output_len()];
            for [azimuth, elevation] in queries {
                filters
                    .query(azimuth, elevation, mode, &mut values, None)
                    .unwrap();
                all.extend_from_slice(&values);
            }
            floats(out, &format!("{prefix}.{suffix}.c32"), &all);
        }
    }
    mradm_dsp::diagnostics::scope("");
}
