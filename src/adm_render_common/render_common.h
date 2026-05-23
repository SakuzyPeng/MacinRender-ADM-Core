#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "adm/logging.h"
#include "adm/scene.h"

namespace mradm::render_common {

struct PreparedObjectBlock {
    std::vector<SceneObjectBlock> sources;
    uint64_t start_sample{0};
    uint64_t end_sample{0};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

[[nodiscard]] PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                                       const SceneObject& object,
                                                       const std::vector<SceneOutputSpeaker>& speakers,
                                                       LogSink& logs,
                                                       std::string_view log_module,
                                                       bool& screen_ref_warned);

template <typename Channel>
[[nodiscard]] uint64_t block_active_length(const Channel& channel, std::size_t block_index) {
    const auto& block = channel.blocks[block_index];
    uint64_t active_end = block.end_sample;
    if (block_index + 1 < channel.blocks.size()) {
        active_end = std::min(active_end, channel.blocks[block_index + 1].start_sample);
    }
    if (active_end <= block.start_sample) {
        return 0;
    }
    return active_end - block.start_sample;
}

template <typename Channel>
[[nodiscard]] uint64_t interpolation_length(const Channel& channel, std::size_t block_index, uint64_t default_interp) {
    const auto& block = channel.blocks[block_index];
    if (block.jump_position || block_index == 0) {
        return 0;
    }
    return std::min(block.interp_length_samples.value_or(default_interp), block_active_length(channel, block_index));
}

template <typename T> [[nodiscard]] T interpolated_scalar(T previous, T current, uint64_t delta, uint64_t interp_len) {
    const auto alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
    return static_cast<T>((static_cast<double>(previous) * (1.0 - alpha)) + (static_cast<double>(current) * alpha));
}

} // namespace mradm::render_common
