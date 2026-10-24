#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "speaker_pcm.h"

// Per-channel gain blocks compiled into a PcmMixer plan, for tests comparing the Rust mixer with
// the frozen legacy accumulation. Production plans are built in Rust mradm-render (4.5, 4.6).
namespace mradm::render_common {

struct BlockGains {
    std::vector<float> gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    bool smoothable_object{false};
    std::optional<uint64_t> interp_length_samples;
};

// One input channel with its full sorted block sequence.
struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::string object_id;
    std::string speaker_label_key;
    std::vector<BlockGains> blocks; // sorted by start_sample
    float output_gain{1.0F};
};

[[nodiscard]] inline Result<PreparedPcmMix>
prepare_speaker_mix(std::vector<ChannelGainInfo> channels, std::size_t inputs, std::size_t outputs) {
    PreparedPcmMix result;
    std::vector<MradmDspMixRow> rows;
    std::vector<MradmDspMixBlock> blocks;
    std::vector<float> gains;
    for (auto& channel : channels) {
        result.channels.push_back(
            {channel.input_channel, std::move(channel.object_id), std::move(channel.speaker_label_key)});
        rows.push_back({channel.input_channel, blocks.size(), channel.blocks.size(), channel.output_gain});
        for (auto& block : channel.blocks) {
            if (block.gains.size() != outputs) {
                return make_error(ErrorCode::invalid_argument, "PCM gain width mismatch");
            }
            // Object-duration clipping can put the end before the start. Preserve an empty block
            // (and its gains) so later blocks retain the legacy interpolation predecessor.
            blocks.push_back({block.start_sample,
                              std::max(block.start_sample, block.end_sample),
                              block.interp_length_samples.value_or(0),
                              (block.jump_position ? 1U : 0U) | (block.smoothable_object ? 2U : 0U) |
                                  (block.interp_length_samples ? 4U : 0U)});
            gains.insert(gains.end(), block.gains.begin(), block.gains.end());
        }
        std::vector<BlockGains>{}.swap(channel.blocks);
    }
    auto prepared = dsp::PcmMixPlan::create(inputs, outputs, rows, blocks, gains);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    result.plan = std::move(*prepared);
    return result;
}

} // namespace mradm::render_common
