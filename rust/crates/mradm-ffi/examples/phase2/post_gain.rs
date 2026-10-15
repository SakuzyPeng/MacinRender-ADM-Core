//! Offline post-processing gain on a fixed grid of measurements (PCM consistency 2.3).
use super::{doubles, floats};
use mradm_dsp::post_gain::{self, RenderGainRequest, Stage};
use std::path::Path;

/// Measured amplitudes: silence floor, dyadic and decimal peaks, and values near 0 dBTP.
const AMPLITUDES: [f64; 12] = [
    1.0e-10,
    1.0e-5,
    0.001,
    0.0625,
    0.1,
    0.25,
    0.5,
    std::f64::consts::FRAC_1_SQRT_2,
    0.89,
    0.999,
    1.0,
    std::f64::consts::SQRT_2,
];
/// Gains in dB, including the established targets and tolerance edges.
const DECIBELS: [f64; 14] = [
    -60.0, -23.0, -20.0, -6.0, -3.0, -1.0, -0.1, -0.01, 0.0, 0.01, 0.1, 1.0, 6.0, 23.7,
];

fn stage(value: Stage) -> f64 {
    match value {
        Stage::Skipped => 0.0,
        Stage::Unchanged => 1.0,
        Stage::Applied => 2.0,
    }
}

pub(super) fn run(out: &Path) {
    let mut conversions: Vec<f64> = AMPLITUDES
        .iter()
        .map(|&a| post_gain::amplitude_to_db(a))
        .collect();
    conversions.extend(DECIBELS.iter().map(|&db| post_gain::db_to_amplitude(db)));
    doubles(out, "post-gain.10-conversions.f64", &conversions);

    // Render staging: every combination of the three automatic stages, measurements
    // across the tolerance edges, and a final gain that does or does not reach 0.01 dB.
    let mut plans = Vec::new();
    let mut gains = Vec::new();
    for measured_lufs in [-30.0, -23.15, -23.05, -16.25] {
        for measured_peak in [-12.0, -1.05, -0.95, 0.75] {
            for flags in 0..8u32 {
                for final_gain_db in [0.0, 0.004, -2.5] {
                    let plan = post_gain::render_gain(&RenderGainRequest {
                        measured_lufs: Some(measured_lufs),
                        measured_peak_dbtp: Some(measured_peak),
                        loudness_target_lufs: -23.0,
                        peak_limit_dbtp: -1.0,
                        final_gain_db,
                        normalize_loudness: flags & 1 != 0,
                        peak_normalize_to_limit: flags & 2 != 0,
                        peak_limit: flags & 4 != 0,
                    });
                    plans.extend([
                        stage(plan.loudness),
                        plan.loudness_db,
                        stage(plan.makeup),
                        plan.makeup_peak_after,
                        plan.makeup_db,
                        stage(plan.ceiling),
                        plan.ceiling_peak_after,
                        plan.ceiling_db,
                        f64::from(u8::from(plan.final_applied)),
                        plan.gain_db,
                        f64::from(u8::from(plan.apply)),
                    ]);
                    gains.push(plan.gain);
                }
            }
        }
    }
    doubles(out, "post-gain.20-render-plan.f64", &plans);
    floats(out, "post-gain.21-render-gain.f32", &gains);

    // apply_loudness_norm and apply_peak_limit.
    let mut standalone = Vec::new();
    let mut standalone_gains = Vec::new();
    for measured in [-40.0, -23.15, -23.05, -22.95, -14.0] {
        let gain = post_gain::loudness_gain(measured, -23.0);
        standalone.extend([gain.gain_db, f64::from(u8::from(gain.apply))]);
        standalone_gains.push(gain.gain);
    }
    for &peak in &AMPLITUDES {
        for target in [-1.0, -0.1, 0.0] {
            let gain = post_gain::peak_limit_gain(peak, target);
            standalone.extend([
                gain.peak_dbtp,
                gain.target_linear,
                gain.gain_db,
                f64::from(u8::from(gain.apply)),
            ]);
            standalone_gains.push(gain.gain);
        }
    }
    doubles(out, "post-gain.30-standalone.f64", &standalone);
    floats(out, "post-gain.31-standalone-gain.f32", &standalone_gains);
}
