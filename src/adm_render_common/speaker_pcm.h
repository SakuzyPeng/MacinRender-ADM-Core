#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adm/render.h"

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
    std::string object_id;          // owning SceneObject::id, for live gain overrides
    std::string speaker_label_key;  // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
    std::vector<BlockGains> blocks; // sorted by start_sample
    float output_gain{1.0F};
};

struct AccumulateContext {
    const float* input{nullptr};
    std::vector<float>* output{nullptr};
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
    uint64_t object_smoothing_frames{0};
    // Optional sample-domain output gain per input channel, after spatial/user gain.
    std::span<const float> live_gains{};
};

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now);

// One channel, without the optional extra object-smoothing stage. This preserves the
// matrix mix's arithmetic and lets streaming backends interleave bounded live curves.
void accumulate_speaker_channel(const ChannelGainInfo& channel,
                                std::size_t& block_index,
                                const AccumulateContext& ctx,
                                uint64_t frames_now);

// Optional per-render stateful DSP. Adds to interleaved output after the gain mix.
// A non-empty processor requires warming from frame zero for cropped output.
using SpeakerBlockProcessor =
    std::function<Result<void>(std::span<const float> input, std::span<float> output, bool end_of_input)>;

[[nodiscard]] Result<RenderMetrics> render_speaker_pcm(const RenderPlan& plan,
                                                       const std::vector<ChannelGainInfo>& gain_matrix,
                                                       uint16_t num_out_ch,
                                                       std::string_view backend,
                                                       ProgressSink& progress,
                                                       LogSink& logs,
                                                       const SpeakerBlockProcessor& process = {});

} // namespace mradm::render_common
