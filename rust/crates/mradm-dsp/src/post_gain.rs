//! Offline post-processing gain: dB/amplitude conversion, the staged render gain
//! (loudness target, peak makeup, peak ceiling, final gain) and the standalone
//! loudness-normalization and peak-limit gains. Conversions take log10/pow from
//! the libm port, so every platform derives the same f32 gain from the same
//! measurement (PCM consistency 2.3). Decisions keep the established tolerances.

/// `20·log10(linear)`; zero gives -inf, as before.
pub fn amplitude_to_db(linear: f64) -> f64 {
    20.0 * libm::log10(linear)
}

/// `10^(db/20)`.
pub fn db_to_amplitude(db: f64) -> f64 {
    libm::pow(10.0, db / 20.0)
}

/// Multiplies every sample by `gain` (no finiteness check: the offline paths keep
/// their established behavior for out-of-range gains).
pub fn apply(samples: &mut [f32], gain: f32) {
    for sample in samples {
        *sample *= gain;
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Stage {
    /// Not requested, or no measurement.
    #[default]
    Skipped,
    /// Requested and measured, but inside the tolerance.
    Unchanged,
    Applied,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RenderGainRequest {
    pub measured_lufs: Option<f64>,
    pub measured_peak_dbtp: Option<f64>,
    pub loudness_target_lufs: f64,
    pub peak_limit_dbtp: f64,
    pub final_gain_db: f64,
    pub normalize_loudness: bool,
    pub peak_normalize_to_limit: bool,
    pub peak_limit: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RenderGainPlan {
    pub loudness: Stage,
    pub loudness_db: f64,
    pub makeup: Stage,
    pub makeup_peak_after: f64,
    pub makeup_db: f64,
    pub ceiling: Stage,
    pub ceiling_peak_after: f64,
    pub ceiling_db: f64,
    pub final_applied: bool,
    pub gain_db: f64,
    /// Whether the file is rewritten; `gain` is 1 otherwise.
    pub apply: bool,
    pub gain: f32,
}

/// Loudness target first, optional peak makeup second, peak ceiling third, then the
/// unconstrained final gain, all summed in dB and applied once.
pub fn render_gain(request: &RenderGainRequest) -> RenderGainPlan {
    let mut plan = RenderGainPlan {
        gain: 1.0,
        ..RenderGainPlan::default()
    };
    let mut gain_db = 0.0;
    if let (true, Some(measured)) = (request.normalize_loudness, request.measured_lufs) {
        let delta = request.loudness_target_lufs - measured;
        plan.loudness_db = delta;
        plan.loudness = if delta.abs() >= 0.1 {
            gain_db += delta;
            Stage::Applied
        } else {
            Stage::Unchanged
        };
    }
    if let (true, Some(peak)) = (request.peak_normalize_to_limit, request.measured_peak_dbtp) {
        let after = peak + gain_db;
        let makeup = request.peak_limit_dbtp - after;
        plan.makeup_peak_after = after;
        plan.makeup_db = makeup;
        plan.makeup = if makeup > 0.1 {
            gain_db += makeup;
            Stage::Applied
        } else {
            Stage::Unchanged
        };
    }
    if let (true, Some(peak)) = (request.peak_limit, request.measured_peak_dbtp) {
        let after = peak + gain_db;
        let headroom = request.peak_limit_dbtp - after;
        // std::min(0.0, headroom): NaN and -0 give +0.
        let clamp = if headroom < 0.0 { headroom } else { 0.0 };
        plan.ceiling_peak_after = after;
        plan.ceiling_db = clamp;
        plan.ceiling = if clamp < -0.1 {
            gain_db += clamp;
            Stage::Applied
        } else {
            Stage::Unchanged
        };
    }
    if request.final_gain_db.abs() >= 1e-9 {
        gain_db += request.final_gain_db;
        plan.final_applied = true;
    }
    plan.gain_db = gain_db;
    if gain_db.abs() >= 0.01 {
        plan.apply = true;
        plan.gain = db_to_amplitude(gain_db) as f32;
    }
    plan
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LoudnessGain {
    pub gain_db: f64,
    pub apply: bool,
    pub gain: f32,
}

/// `apply_loudness_norm`: no rewrite within 0.1 LU of the target.
pub fn loudness_gain(measured_lufs: f64, target_lufs: f64) -> LoudnessGain {
    let gain_db = target_lufs - measured_lufs;
    if gain_db.abs() < 0.1 {
        return LoudnessGain {
            gain_db,
            apply: false,
            gain: 1.0,
        };
    }
    LoudnessGain {
        gain_db,
        apply: true,
        gain: db_to_amplitude(gain_db) as f32,
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PeakLimitGain {
    pub peak_dbtp: f64,
    pub target_linear: f64,
    pub apply: bool,
    pub gain: f32,
    pub gain_db: f64,
}

/// `apply_peak_limit`: no rewrite unless the peak exceeds the target by more than 0.1 %.
pub fn peak_limit_gain(peak_linear: f64, target_dbtp: f64) -> PeakLimitGain {
    // std::max(1e-10, peak): NaN gives the floor.
    let floored = if 1.0e-10 < peak_linear {
        peak_linear
    } else {
        1.0e-10
    };
    let target_linear = db_to_amplitude(target_dbtp);
    let mut result = PeakLimitGain {
        peak_dbtp: amplitude_to_db(floored),
        target_linear,
        apply: false,
        gain: 1.0,
        gain_db: 0.0,
    };
    if peak_linear <= target_linear * 1.001 {
        return result;
    }
    result.apply = true;
    result.gain = (target_linear / peak_linear) as f32;
    result.gain_db = amplitude_to_db(f64::from(result.gain));
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> RenderGainRequest {
        RenderGainRequest {
            measured_lufs: Some(-20.0),
            measured_peak_dbtp: Some(-3.0),
            loudness_target_lufs: -23.0,
            peak_limit_dbtp: -1.0,
            ..RenderGainRequest::default()
        }
    }

    #[test]
    fn conversions_round_trip_exact_powers() {
        assert_eq!(amplitude_to_db(1.0), 0.0);
        assert_eq!(amplitude_to_db(10.0), 20.0);
        assert_eq!(amplitude_to_db(0.0), f64::NEG_INFINITY);
        assert_eq!(db_to_amplitude(0.0), 1.0);
        assert_eq!(db_to_amplitude(-20.0), 0.1);
    }

    #[test]
    fn stages_follow_the_established_order_and_tolerances() {
        let plan = render_gain(&RenderGainRequest {
            normalize_loudness: true,
            peak_limit: true,
            ..request()
        });
        assert_eq!(plan.loudness, Stage::Applied);
        assert_eq!(plan.loudness_db, -3.0);
        assert_eq!(plan.makeup, Stage::Skipped);
        // -3 dBTP lowered by 3 dB stays under the -1 dBTP ceiling.
        assert_eq!(plan.ceiling, Stage::Unchanged);
        assert_eq!(plan.ceiling_peak_after, -6.0);
        assert!(plan.apply);
        assert_eq!(plan.gain, db_to_amplitude(-3.0) as f32);

        let plan = render_gain(&RenderGainRequest {
            measured_lufs: Some(-23.05),
            normalize_loudness: true,
            peak_normalize_to_limit: true,
            peak_limit: true,
            ..request()
        });
        assert_eq!(plan.loudness, Stage::Unchanged);
        assert_eq!(plan.makeup, Stage::Applied);
        assert_eq!(plan.makeup_db, 2.0);
        assert_eq!(plan.ceiling, Stage::Unchanged);
        assert_eq!(plan.ceiling_db, 0.0);
        assert_eq!(plan.gain_db, 2.0);

        let plan = render_gain(&RenderGainRequest {
            measured_peak_dbtp: Some(0.5),
            peak_limit: true,
            final_gain_db: 6.0,
            ..request()
        });
        assert_eq!(plan.loudness, Stage::Skipped);
        assert_eq!(plan.ceiling, Stage::Applied);
        assert_eq!(plan.ceiling_db, -1.5);
        assert!(plan.final_applied);
        assert_eq!(plan.gain_db, 4.5);

        let plan = render_gain(&RenderGainRequest {
            final_gain_db: 0.005,
            ..request()
        });
        assert!(plan.final_applied);
        assert!(!plan.apply);
        assert_eq!(plan.gain, 1.0);
    }

    #[test]
    fn standalone_gains_keep_their_tolerances() {
        let unchanged = loudness_gain(-23.05, -23.0);
        assert!(!unchanged.apply);
        assert_eq!(unchanged.gain, 1.0);
        let applied = loudness_gain(-17.0, -23.0);
        assert!(applied.apply);
        assert_eq!(applied.gain, db_to_amplitude(-6.0) as f32);

        let within = peak_limit_gain(0.89, -1.0);
        assert!(!within.apply);
        let over = peak_limit_gain(1.0, -1.0);
        assert!(over.apply);
        assert_eq!(over.peak_dbtp, 0.0);
        assert_eq!(over.gain, db_to_amplitude(-1.0) as f32);
        let silent = peak_limit_gain(0.0, -1.0);
        assert!(!silent.apply);
        assert_eq!(silent.peak_dbtp, -200.0);
    }
}
