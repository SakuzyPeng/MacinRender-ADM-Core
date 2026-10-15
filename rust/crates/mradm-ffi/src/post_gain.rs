//! Private boundary for offline post-processing gain (PCM consistency 2.3).
use crate::{boundary, checked::*};
use mradm_dsp::post_gain::{self, RenderGainRequest, Stage};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RenderGainInput {
    measured_lufs: f64,
    measured_peak_dbtp: f64,
    loudness_target_lufs: f64,
    peak_limit_dbtp: f64,
    final_gain_db: f64,
    has_lufs: u32,
    has_peak: u32,
    normalize_loudness: u32,
    peak_normalize_to_limit: u32,
    peak_limit: u32,
}

/// Stages: 0 skipped, 1 unchanged (inside the tolerance), 2 applied.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RenderGainOutput {
    loudness_db: f64,
    makeup_peak_after: f64,
    makeup_db: f64,
    ceiling_peak_after: f64,
    ceiling_db: f64,
    gain_db: f64,
    loudness: u32,
    makeup: u32,
    ceiling: u32,
    final_applied: u32,
    apply: u32,
    gain: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct LoudnessGainOutput {
    gain_db: f64,
    apply: u32,
    gain: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PeakLimitOutput {
    peak_dbtp: f64,
    target_linear: f64,
    gain_db: f64,
    apply: u32,
    gain: f32,
}

fn stage(value: Stage) -> u32 {
    match value {
        Stage::Skipped => 0,
        Stage::Unchanged => 1,
        Stage::Applied => 2,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mradm_dsp_amplitude_to_db(linear: f64) -> f64 {
    post_gain::amplitude_to_db(linear)
}

#[unsafe(no_mangle)]
pub extern "C" fn mradm_dsp_db_to_amplitude(db: f64) -> f64 {
    post_gain::db_to_amplitude(db)
}

/// Offline post-processing gain; `length` counts floats, empty buffers may be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_apply_gain(samples: *mut f32, length: usize, gain: f32) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        post_gain::apply(output(samples, length)?, gain);
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_render_gain(
    request: *const RenderGainInput,
    plan: *mut RenderGainOutput,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(request, 1, plan, 1)?;
        let source = input(request, 1)?[0];
        for value in [
            source.has_lufs,
            source.has_peak,
            source.normalize_loudness,
            source.peak_normalize_to_limit,
            source.peak_limit,
        ] {
            flag(value)?;
        }
        let staged = post_gain::render_gain(&RenderGainRequest {
            measured_lufs: (source.has_lufs != 0).then_some(source.measured_lufs),
            measured_peak_dbtp: (source.has_peak != 0).then_some(source.measured_peak_dbtp),
            loudness_target_lufs: source.loudness_target_lufs,
            peak_limit_dbtp: source.peak_limit_dbtp,
            final_gain_db: source.final_gain_db,
            normalize_loudness: source.normalize_loudness != 0,
            peak_normalize_to_limit: source.peak_normalize_to_limit != 0,
            peak_limit: source.peak_limit != 0,
        });
        output(plan, 1)?[0] = RenderGainOutput {
            loudness_db: staged.loudness_db,
            makeup_peak_after: staged.makeup_peak_after,
            makeup_db: staged.makeup_db,
            ceiling_peak_after: staged.ceiling_peak_after,
            ceiling_db: staged.ceiling_db,
            gain_db: staged.gain_db,
            loudness: stage(staged.loudness),
            makeup: stage(staged.makeup),
            ceiling: stage(staged.ceiling),
            final_applied: u32::from(staged.final_applied),
            apply: u32::from(staged.apply),
            gain: staged.gain,
        };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_loudness_gain(
    measured_lufs: f64,
    target_lufs: f64,
    result: *mut LoudnessGainOutput,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        let gain = post_gain::loudness_gain(measured_lufs, target_lufs);
        output(result, 1)?[0] = LoudnessGainOutput {
            gain_db: gain.gain_db,
            apply: u32::from(gain.apply),
            gain: gain.gain,
        };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_peak_limit_gain(
    peak_linear: f64,
    target_dbtp: f64,
    result: *mut PeakLimitOutput,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        let gain = post_gain::peak_limit_gain(peak_linear, target_dbtp);
        output(result, 1)?[0] = PeakLimitOutput {
            peak_dbtp: gain.peak_dbtp,
            target_linear: gain.target_linear,
            gain_db: gain.gain_db,
            apply: u32::from(gain.apply),
            gain: gain.gain,
        };
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_null_and_overlapping_buffers() {
        let input = RenderGainInput::default();
        assert_ne!(
            unsafe { mradm_dsp_render_gain(&input, std::ptr::null_mut()) },
            0
        );
        assert_ne!(
            unsafe { mradm_dsp_loudness_gain(-20.0, -23.0, std::ptr::null_mut()) },
            0
        );
        let bad = RenderGainInput {
            has_lufs: 2,
            ..input
        };
        let mut output = RenderGainOutput::default();
        assert_ne!(unsafe { mradm_dsp_render_gain(&bad, &mut output) }, 0);
    }

    #[test]
    fn forwards_the_plan() {
        let input = RenderGainInput {
            measured_lufs: -17.0,
            loudness_target_lufs: -23.0,
            has_lufs: 1,
            normalize_loudness: 1,
            ..RenderGainInput::default()
        };
        let mut output = RenderGainOutput::default();
        assert_eq!(unsafe { mradm_dsp_render_gain(&input, &mut output) }, 0);
        assert_eq!((output.loudness, output.apply), (2, 1));
        assert_eq!(output.gain_db, -6.0);
        assert_eq!(output.gain, post_gain::db_to_amplitude(-6.0) as f32);
        let mut peak = PeakLimitOutput::default();
        assert_eq!(unsafe { mradm_dsp_peak_limit_gain(2.0, 0.0, &mut peak) }, 0);
        assert_eq!((peak.apply, peak.gain), (1, 0.5));
    }
}
