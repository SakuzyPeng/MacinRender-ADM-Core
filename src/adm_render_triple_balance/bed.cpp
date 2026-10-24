#include "bed.h"

#include <array>
#include <cstdint>

#include "triple_balance.h"

namespace mradm::triple_balance {
namespace {
constexpr std::array<std::string_view, 10> k_labels{
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
}

std::optional<std::size_t> bed_channel(const SceneDirectSpeakersBlock& block) {
    if (block.speaker_labels.size() == 1) {
        for (std::size_t index = 0; index < k_labels.size(); ++index) {
            if (block.speaker_labels.front() == k_labels.at(index)) {
                return index;
            }
        }
    }
    return std::nullopt;
}

Result<std::vector<float>>
bed_gains(const SceneDirectSpeakersBlock& block, std::string_view layout, LfeRoutingMode lfe_mode) {
    const auto index = bed_channel(block);
    const auto code = dsp::tb_layout(layout);
    if (!index || !code) {
        return make_error(ErrorCode::unsupported, "room bed requires verified labels and 7.1.4/9.1.6/22.2 output");
    }
    std::vector<float> gains(dsp::tb_channels(*code));
    auto routed = dsp::tb_status(mradm_dsp_tb_bed_gains,
                                 *code,
                                 static_cast<uint32_t>(*index),
                                 lfe_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                 gains.data(),
                                 gains.size());
    if (!routed) {
        return tl::unexpected{routed.error()};
    }
    return gains;
}
} // namespace mradm::triple_balance
