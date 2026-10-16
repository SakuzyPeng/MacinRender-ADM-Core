#pragma once

#include <array>
#include <cstddef>
#include <exception>
#include <span>
#include <utility>

#include "stream_dsp_ffi.h"

// File-streaming and monitor arithmetic (PCM consistency 3.1). Callers own the buffers and sizes;
// a rejected call is a programming error inside a prepared stream (ADR 0005), so it terminates.
namespace mradm::dsp {

inline void stream_checked(int32_t status) noexcept {
    if (status != 0) {
        std::terminate();
    }
}

// mix[2f + c] += source_c[f] * gain[f] for the first `frames` frames.
inline void accumulate_stereo(std::span<float> mix,
                              std::span<const float> left,
                              std::span<const float> right,
                              std::span<const float> gain,
                              std::size_t frames) noexcept {
    if (left.size() < frames || right.size() < frames || gain.size() < frames) {
        std::terminate();
    }
    stream_checked(mradm_dsp_stream_accumulate(mix.data(), mix.size(), left.data(), right.data(), gain.data(), frames));
}

// Linear blend of one interleaved stereo block from the outgoing into the incoming source graph.
inline void
topology_crossfade(std::span<const float> outgoing, std::span<float> incoming, std::size_t frames) noexcept {
    if (outgoing.size() / 2U < frames || incoming.size() / 2U < frames) {
        std::terminate();
    }
    stream_checked(mradm_dsp_stream_topology_crossfade(outgoing.data(), incoming.data(), frames));
}

[[nodiscard]] inline float extent_spread_degrees(float width, float height, float depth) noexcept {
    return mradm_dsp_extent_spread_degrees(width, height, depth);
}

[[nodiscard]] inline float live_gain(float db) noexcept {
    return mradm_dsp_live_gain(db);
}

[[nodiscard]] inline std::pair<float, float> monitor_stereo_fold(float azimuth) noexcept {
    std::array<float, 2> gains{};
    stream_checked(mradm_dsp_monitor_stereo_fold(azimuth, gains.data()));
    return {gains[0], gains[1]};
}

} // namespace mradm::dsp
