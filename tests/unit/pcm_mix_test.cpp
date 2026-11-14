// Speaker mix timeline regressions for clipped ADM blocks (the legacy C++ PCM mix comparison was retired
// with the frozen references on 2026-11-14; mradm-dsp tests/pcm_mix.rs covers the mixer arithmetic).
#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pcm_mix.h"
#include "speaker_mix.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(mradm::Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

// Blocks clipped by the object duration in the Rust backends: mradm-render tests/vbap.rs, ear.rs.
void check_clipped_timeline() {
    mradm::render_common::ChannelGainInfo channel;
    // Keep clipped blocks as interpolation predecessors when another object uses the same input channel.
    channel.blocks = {{{1.0F}, 0U, 2U, true, true, std::nullopt},
                      {{0.5F}, 4U, 2U, true, true, std::nullopt},
                      {{2.0F}, 6U, 10U, false, true, 4U},
                      {{3.0F}, 12U, 2U, true, true, std::nullopt}};
    auto compiled = take(mradm::render_common::prepare_speaker_mix({channel}, 1U, 1U));
    const std::array<float, 16> expected{
        1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.875F, 1.25F, 1.625F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    for (const std::size_t chunk : {1U, 3U, 16U}) {
        mradm::dsp::PcmMixer mix(compiled.plan, chunk, 0U, false);
        const std::vector<float> input(chunk, 1.0F);
        std::array<float, 16> output{};
        for (std::size_t start = 0; start < output.size(); start += chunk) {
            const auto frames = std::min(chunk, output.size() - start);
            mix.speaker(input, std::span{output}.subspan(start, frames), {}, start, frames);
        }
        require(output == expected, "clipped blocks must stay silent and retain interpolation history");
    }
}
} // namespace

int main() {
    try {
        check_clipped_timeline();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
