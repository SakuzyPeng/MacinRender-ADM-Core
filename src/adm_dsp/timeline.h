#pragma once

#include <cstdint>
#include <optional>

#include "timeline_ffi.h"

// One seconds-to-frame rule for every entry that names a time in seconds (PCM consistency 3.6):
// the output trim, the preview window and Monitor seek / loop points.
namespace mradm::dsp {

// The frame nearest to seconds * sample_rate, halfway cases away from zero, saturating at
// UINT64_MAX; nullopt for a non-finite or negative time or a zero rate.
[[nodiscard]] inline std::optional<std::uint64_t> seconds_to_frame(double seconds, std::uint32_t sample_rate) noexcept {
    std::uint64_t frame = 0;
    if (mradm_dsp_seconds_to_frame(seconds, sample_rate, &frame) != 0) {
        return std::nullopt;
    }
    return frame;
}

} // namespace mradm::dsp
