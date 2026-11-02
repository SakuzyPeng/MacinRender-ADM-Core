#pragma once
#include <stddef.h>
#include <stdint.h>
// Offline post-processing gain (PCM consistency 2.3); see mradm-dsp `post_gain`.
// dB/amplitude conversions use the libm port. Status values follow adm::ErrorCode.
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRenderGainInput {
    double measured_lufs;
    double measured_peak_dbtp;
    double loudness_target_lufs;
    double peak_limit_dbtp;
    double final_gain_db;
    uint32_t has_lufs;
    uint32_t has_peak;
    uint32_t normalize_loudness;
    uint32_t peak_normalize_to_limit;
    uint32_t peak_limit;
} MradmRenderGainInput;
// Stages: 0 skipped, 1 unchanged (inside the tolerance), 2 applied.
typedef struct MradmRenderGainOutput {
    double loudness_db;
    double makeup_peak_after;
    double makeup_db;
    double ceiling_peak_after;
    double ceiling_db;
    double gain_db;
    uint32_t loudness;
    uint32_t makeup;
    uint32_t ceiling;
    uint32_t final_applied;
    uint32_t apply;
    float gain;
} MradmRenderGainOutput;
typedef struct MradmLoudnessGainOutput {
    double gain_db;
    uint32_t apply;
    float gain;
} MradmLoudnessGainOutput;
typedef struct MradmPeakLimitOutput {
    double peak_dbtp;
    double target_linear;
    double gain_db;
    uint32_t apply;
    float gain;
} MradmPeakLimitOutput;
// cppcheck-suppress-end unusedStructMember

// 20·log10(linear) and 10^(db/20).
double mradm_dsp_amplitude_to_db(double linear);
double mradm_dsp_db_to_amplitude(double db);
// Multiplies `length` floats in place; empty buffers may be null.
int32_t mradm_dsp_apply_gain(float* samples, size_t length, float gain);
// Loudness target, peak makeup, peak ceiling and final gain summed in dB.
int32_t mradm_dsp_render_gain(const MradmRenderGainInput* request, MradmRenderGainOutput* plan);
// apply_loudness_norm / apply_peak_limit gains and their tolerances.
int32_t mradm_dsp_loudness_gain(double measured_lufs, double target_lufs, MradmLoudnessGainOutput* result);
int32_t mradm_dsp_peak_limit_gain(double peak_linear, double target_dbtp, MradmPeakLimitOutput* result);
#ifdef __cplusplus
}
#endif
