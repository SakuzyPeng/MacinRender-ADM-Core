#pragma once

#include "stream_dsp_ffi.h"

// The monitor's live gain multiplier of a dB override (PCM consistency 3.1).
namespace mradm::dsp {

[[nodiscard]] inline float live_gain(float db) noexcept {
    return mradm_dsp_live_gain(db);
}

} // namespace mradm::dsp
