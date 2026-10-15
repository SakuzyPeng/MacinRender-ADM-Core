#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "adm/errors.h"

#include "post_gain_ffi.h"

// Offline post-processing gain (PCM consistency 2.3): dB/amplitude conversion and
// gain decisions in Rust with the libm port; callers keep logging and file I/O.
namespace mradm::dsp {

[[nodiscard]] inline double amplitude_to_db(double linear) noexcept {
    return mradm_dsp_amplitude_to_db(linear);
}

[[nodiscard]] inline double db_to_amplitude(double db) noexcept {
    return mradm_dsp_db_to_amplitude(db);
}

[[nodiscard]] inline Result<void> apply_gain(std::span<float> samples, float gain) {
    if (mradm_dsp_apply_gain(samples.data(), samples.size(), gain) != 0) {
        return make_error(ErrorCode::invalid_argument, "invalid post-processing gain buffer");
    }
    return {};
}

enum class GainStage : uint32_t { skipped, unchanged, applied };

struct RenderGainRequest {
    std::optional<double> measured_lufs;
    std::optional<double> measured_peak_dbtp;
    double loudness_target_lufs = 0.0;
    double peak_limit_dbtp = 0.0;
    double final_gain_db = 0.0;
    bool normalize_loudness = false;
    bool peak_normalize_to_limit = false;
    bool peak_limit = false;
};

struct RenderGainPlan {
    GainStage loudness = GainStage::skipped;
    double loudness_db = 0.0;
    GainStage makeup = GainStage::skipped;
    double makeup_peak_after = 0.0;
    double makeup_db = 0.0;
    GainStage ceiling = GainStage::skipped;
    double ceiling_peak_after = 0.0;
    double ceiling_db = 0.0;
    bool final_applied = false;
    double gain_db = 0.0;
    bool apply = false;
    float gain = 1.0F;
};

// Loudness target, optional peak makeup, peak ceiling, then the unconstrained final gain.
[[nodiscard]] inline Result<RenderGainPlan> render_gain(const RenderGainRequest& request) {
    const MradmRenderGainInput input{request.measured_lufs.value_or(0.0),
                                     request.measured_peak_dbtp.value_or(0.0),
                                     request.loudness_target_lufs,
                                     request.peak_limit_dbtp,
                                     request.final_gain_db,
                                     request.measured_lufs.has_value() ? 1U : 0U,
                                     request.measured_peak_dbtp.has_value() ? 1U : 0U,
                                     request.normalize_loudness ? 1U : 0U,
                                     request.peak_normalize_to_limit ? 1U : 0U,
                                     request.peak_limit ? 1U : 0U};
    MradmRenderGainOutput output{};
    if (mradm_dsp_render_gain(&input, &output) != 0) {
        return make_error(ErrorCode::internal_error, "post-processing gain plan failed");
    }
    return RenderGainPlan{static_cast<GainStage>(output.loudness),
                          output.loudness_db,
                          static_cast<GainStage>(output.makeup),
                          output.makeup_peak_after,
                          output.makeup_db,
                          static_cast<GainStage>(output.ceiling),
                          output.ceiling_peak_after,
                          output.ceiling_db,
                          output.final_applied != 0U,
                          output.gain_db,
                          output.apply != 0U,
                          output.gain};
}

struct LoudnessGain {
    double gain_db = 0.0;
    bool apply = false;
    float gain = 1.0F;
};

// apply_loudness_norm: unchanged within 0.1 LU of the target.
[[nodiscard]] inline Result<LoudnessGain> loudness_gain(double measured_lufs, double target_lufs) {
    MradmLoudnessGainOutput output{};
    if (mradm_dsp_loudness_gain(measured_lufs, target_lufs, &output) != 0) {
        return make_error(ErrorCode::internal_error, "loudness gain failed");
    }
    return LoudnessGain{output.gain_db, output.apply != 0U, output.gain};
}

struct PeakLimitGain {
    double peak_dbtp = 0.0;
    double target_linear = 0.0;
    double gain_db = 0.0;
    bool apply = false;
    float gain = 1.0F;
};

// apply_peak_limit: unchanged unless the peak exceeds the target by more than 0.1 %.
[[nodiscard]] inline Result<PeakLimitGain> peak_limit_gain(double peak_linear, double target_dbtp) {
    MradmPeakLimitOutput output{};
    if (mradm_dsp_peak_limit_gain(peak_linear, target_dbtp, &output) != 0) {
        return make_error(ErrorCode::internal_error, "peak limit gain failed");
    }
    return PeakLimitGain{output.peak_dbtp, output.target_linear, output.gain_db, output.apply != 0U, output.gain};
}

} // namespace mradm::dsp
