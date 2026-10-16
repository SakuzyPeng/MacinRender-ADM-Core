#pragma once
#include <stdint.h>
// Seconds to sample frames (PCM consistency 3.6); see mradm-dsp `timeline`. The frame nearest to
// the binary64 product seconds * sample_rate, ties away from zero, saturating at UINT64_MAX.
// Fails for a non-finite or negative time or a zero rate. Status values follow adm::ErrorCode.
#ifdef __cplusplus
extern "C" {
#endif
int32_t mradm_dsp_seconds_to_frame(double seconds, uint32_t sample_rate, uint64_t* frame);
#ifdef __cplusplus
}
#endif
