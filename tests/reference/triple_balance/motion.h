#pragma once
#include <cmath>
#include <limits>

#include "panner.h"
#include "speaker_pcm.h"
namespace mradm::triple_balance_legacy {
using render_common::BlockGains;
[[nodiscard]] Result<std::vector<BlockGains>> triple_balance_motion_blocks(const SceneTrackRef& track,
                                                                           const SceneObject& object,
                                                                           std::string_view layout_id,
                                                                           uint64_t file_frames) {
    if (track.blocks.empty()) {
        return std::vector<BlockGains>{};
    }
    constexpr uint64_t k_control_frames = 512;
    constexpr float k_position_tau_frames = 1200.0F;
    const float alpha = 1.0F - std::exp(-static_cast<float>(k_control_frames) / k_position_tau_frames);
    const auto& first = track.blocks.front();
    if (first.start_sample != 0 || object.end_sample < file_frames) {
        return make_error(ErrorCode::unsupported,
                          "triple-balance requires an object present from frame 0 through the file end");
    }
    // The reference smooths float32 internal room coordinates. Double state
    // eventually rounds across half-code boundaries (notably Z=0.5), while the
    // real float state can remain one ULP below the target indefinitely.
    std::array<float, 3> state{(first.position.x + 1) * 0.5F, (1 - first.position.y) * 0.5F, first.position.z};
    std::array<float, 3> target = state;
    // The compatibility profile ignores audioBlockFormat/gain for
    // Objects. Static 0/-6/-12 dB probes and a changing-gain probe both retain
    // unit gain in the exported speaker PCM. This is specific to this
    // compatibility mode; the other backends retain their own ADM gain semantics.
    auto initial = triple_balance_legacy::point_gains(first.position, 1.0F, layout_id);
    if (!initial) {
        return tl::unexpected{initial.error()};
    }
    std::vector<BlockGains> result;
    result.push_back({std::move(*initial), 0, std::numeric_limits<uint64_t>::max(), true, true, std::nullopt});
    if (track.blocks.size() == 1) {
        return result;
    }
    std::size_t next_event = 1;
    for (uint64_t control_start = k_control_frames; control_start < file_frames; control_start += k_control_frames) {
        if (next_event < track.blocks.size() &&
            track.blocks[next_event].start_sample < control_start + k_control_frames) {
            const auto& event = track.blocks[next_event];
            if (event.start_sample < control_start ||
                (next_event + 1 < track.blocks.size() &&
                 track.blocks[next_event + 1].start_sample < control_start + k_control_frames)) {
                return make_error(ErrorCode::unsupported,
                                  "triple-balance supports at most one metadata update per 512-frame control block");
            }
            target = {(event.position.x + 1) * 0.5F, (1 - event.position.y) * 0.5F, event.position.z};
            ++next_event;
        }
        state = {state[0] + (alpha * (target[0] - state[0])),
                 state[1] + (alpha * (target[1] - state[1])),
                 state[2] + (alpha * (target[2] - state[2]))};
        SceneBlockPosition position;
        position.cartesian = true;
        position.x = (state[0] * 2) - 1;
        position.y = 1 - (state[1] * 2);
        position.z = state[2];
        auto gains = triple_balance_legacy::point_gains(position, 1.0F, layout_id);
        if (!gains) {
            return tl::unexpected{gains.error()};
        }
        if (*gains != result.back().gains) {
            result.push_back({std::move(*gains),
                              control_start,
                              std::numeric_limits<uint64_t>::max(),
                              false,
                              true,
                              k_control_frames});
        }
    }
    if (next_event != track.blocks.size()) {
        return make_error(ErrorCode::unsupported, "triple-balance metadata update falls outside the audio timeline");
    }
    return result;
}

} // namespace mradm::triple_balance_legacy
