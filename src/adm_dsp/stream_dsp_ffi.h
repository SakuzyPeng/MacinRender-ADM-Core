#pragma once
// The monitor's live gain (PCM consistency 3.1); see mradm-dsp `stream_dsp::live_gain` (libm powf).
#ifdef __cplusplus
extern "C" {
#endif
float mradm_dsp_live_gain(float db);
#ifdef __cplusplus
}
#endif
