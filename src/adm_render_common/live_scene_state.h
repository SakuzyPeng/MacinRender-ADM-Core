#pragma once

#include <cmath>
#include <cstdint>

#include "live_scene_renderer.h"

namespace mradm::live_scene {

[[nodiscard]] inline ObjectState default_state(const ElementDescriptor& descriptor) {
    ObjectState state;
    state.valid_fields = k_known_state_fields & ~state_position;
    if (descriptor.role == ElementRole::object) {
        state.valid_fields |= state_position;
    }
    if (descriptor.has_position) {
        state.x = descriptor.x;
        state.y = descriptor.y;
        state.z = descriptor.z;
    }
    return state;
}

inline void copy_state_fields(ObjectState& destination, const ObjectState& source, std::uint64_t requested_fields) {
    const auto fields = source.valid_fields & requested_fields;
    if ((fields & state_active) != 0U) {
        destination.active = source.active;
    }
    if ((fields & state_linear_gain) != 0U) {
        destination.linear_gain = source.linear_gain;
    }
    if ((fields & state_position) != 0U) {
        destination.x = source.x;
        destination.y = source.y;
        destination.z = source.z;
    }
    if ((fields & state_extent) != 0U) {
        destination.width = source.width;
        destination.height = source.height;
        destination.depth = source.depth;
    }
    if ((fields & state_diffuse) != 0U) {
        destination.diffuse = source.diffuse;
    }
    if ((fields & state_divergence) != 0U) {
        destination.divergence = source.divergence;
    }
    if ((fields & state_channel_lock) != 0U) {
        destination.channel_lock = source.channel_lock;
    }
    if ((fields & state_screen_reference) != 0U) {
        destination.screen_reference = source.screen_reference;
    }
    if ((fields & state_head_locked) != 0U) {
        destination.head_locked = source.head_locked;
    }
    if ((fields & state_divergence_range) != 0U) {
        destination.divergence_azimuth_range = source.divergence_azimuth_range;
        destination.divergence_position_range = source.divergence_position_range;
    }
    if ((fields & state_channel_lock_max_distance) != 0U) {
        destination.channel_lock_max_distance = source.channel_lock_max_distance;
    }
    destination.valid_fields |= fields;
}

// Producer ranges are validated upstream. Policy-expanded coordinates/extents may exceed them;
// this numerical boundary checks finiteness without narrowing the policy domain.
[[nodiscard]] inline bool valid_state(const ObjectState& state) noexcept {
    const auto coordinate = [](float v) { return std::isfinite(v); };
    const auto finite = [](float v) { return std::isfinite(v); };
    if ((state.valid_fields & ~k_known_state_fields) != 0U) {
        return false;
    }
    if ((state.valid_fields & state_linear_gain) != 0U &&
        (!std::isfinite(state.linear_gain) || state.linear_gain < 0)) {
        return false;
    }
    if ((state.valid_fields & state_position) != 0U &&
        (!coordinate(state.x) || !coordinate(state.y) || !coordinate(state.z))) {
        return false;
    }
    if ((state.valid_fields & state_extent) != 0U &&
        (!finite(state.width) || !finite(state.height) || !finite(state.depth))) {
        return false;
    }
    if ((state.valid_fields & state_diffuse) != 0U && !finite(state.diffuse)) {
        return false;
    }
    if ((state.valid_fields & state_divergence) != 0U && !finite(state.divergence)) {
        return false;
    }
    if ((state.valid_fields & state_divergence_range) != 0U &&
        (!std::isfinite(state.divergence_azimuth_range) || state.divergence_azimuth_range < 0 ||
         !std::isfinite(state.divergence_position_range) || state.divergence_position_range < 0)) {
        return false;
    }
    return (state.valid_fields & state_channel_lock_max_distance) == 0U || !state.channel_lock_max_distance ||
           (std::isfinite(*state.channel_lock_max_distance) && *state.channel_lock_max_distance >= 0);
}

} // namespace mradm::live_scene
