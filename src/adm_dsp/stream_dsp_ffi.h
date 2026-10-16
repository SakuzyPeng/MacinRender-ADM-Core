#pragma once
#include <stddef.h>
#include <stdint.h>
// File-streaming and monitor arithmetic (PCM consistency 3.1); see mradm-dsp `stream_dsp` and
// `monitor::stereo_fold`. Products and sums are rounded separately; trigonometry and powf use
// the libm port. Status values follow adm::ErrorCode.
#ifdef __cplusplus
extern "C" {
#endif
// mix[2f + c] += source_c[f] * gain[f]; left/right/gain hold `frames` floats and must not overlap mix.
int32_t mradm_dsp_stream_accumulate(
    float* mix, size_t mix_length, const float* left, const float* right, const float* gain, size_t frames);
// Blends 2 * frames interleaved stereo samples of `outgoing` into `incoming`, mix = f / (frames - 1).
int32_t mradm_dsp_stream_topology_crossfade(const float* outgoing, float* incoming, size_t frames);
float mradm_dsp_extent_spread_degrees(float width, float height, float depth);
float mradm_dsp_live_gain(float db);
// Constant-power {L, R} fold gains of a speaker at `azimuth` degrees into gains[2].
int32_t mradm_dsp_monitor_stereo_fold(float azimuth, float* gains);
#ifdef __cplusplus
}
#endif
