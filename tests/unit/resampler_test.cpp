#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "resampler.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::vector<float> render(mradm::dsp::Resampler& resampler, std::span<const float> input) {
    std::vector<float> result;
    std::array<float, 34> output{};
    std::size_t offset = 0U;
    while (offset < input.size()) {
        const auto samples = std::min<std::size_t>(94U, input.size() - offset);
        auto progress = resampler.process(input.subspan(offset, samples), output);
        require(progress.has_value(), "Rust resampler process failed");
        offset += progress->input_frames * 2U;
        result.insert(
            result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(progress->output_frames * 2U));
        require(progress->input_frames != 0U || progress->output_frames != 0U, "resampler made no progress");
    }
    while (true) {
        auto frames = resampler.finish(output);
        require(frames.has_value(), "Rust resampler finish failed");
        if (*frames == 0U) {
            break;
        }
        result.insert(result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(*frames * 2U));
    }
    return result;
}
} // namespace
int main() {
    try {
        for (const auto rate : {8000U, 44100U, 44101U, 48000U, 96000U, 192000U}) {
            auto created = mradm::dsp::Resampler::create(2U, 48000U, rate);
            require(created.has_value(), "create resampler");
            auto resampler = std::move(*created);
            std::vector<float> input(2050U, 0.0F);
            input.at(1024U) = 1.0F;
            const auto reference = render(resampler, input);
            const auto frames = (1025ULL * rate + 47999ULL) / 48000ULL;
            require(reference.size() == frames * 2U, "FFI output length is ceil of the full rational timeline");
            for (std::size_t i = 0; i < reference.size(); i += 2U) {
                require(std::isfinite(reference[i]) && reference[i + 1U] == 0.0F,
                        "finite PCM and isolated right channel");
            }
            require(resampler.reset().has_value(), "reset resampler");
            std::array<float, 16> scratch{};
            const std::array<float, 2> invalid{std::numeric_limits<float>::quiet_NaN(), 0.0F};
            require(!resampler.process(invalid, scratch), "reject nonfinite PCM");
            require(!resampler.process(std::span{input}.first(1U), scratch), "reject incomplete frames");
            require(render(resampler, input) == reference, "invalid calls leave reset history intact");
            require(!resampler.process(std::span{input}.first(2U), scratch), "reject input after EOS");
        }
        require(!mradm::dsp::Resampler::create(0U, 48000U, 44100U), "reject zero channels");
        int sentinel = 0;
        void* handle = &sentinel;
        require(mradm_dsp_resampler_create(2U, 0U, 44100U, &handle, nullptr, 0U) == 1 && handle == nullptr,
                "failed FFI creation clears output");
        mradm_dsp_resampler_destroy(handle);
        std::cout << "Rust resampler FFI and ownership tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
