// EarPostProcessor against a direct time-domain FIR evaluation (the legacy C++ decorrelator comparison
// was retired with the frozen references on 2026-11-14; mradm-dsp tests/ear_post.rs covers the rest).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ear_post.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T take(mradm::Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}
std::vector<float> flatten(const std::vector<std::vector<float>>& filters) {
    std::vector<float> result;
    result.reserve(filters.size() * 512U);
    for (const auto& row : filters) {
        require(row.size() == 512U, "unexpected libear FIR length");
        result.insert(result.end(), row.begin(), row.end());
    }
    return result;
}
float signal(std::size_t sample, std::size_t seed) {
    return static_cast<float>(static_cast<int>((sample * 17U + seed) % 31U) - 15) / 64.0F;
}

// Fixed, dense, exactly representable FIRs exercise convolution independently of filter design.
std::vector<std::vector<float>> fixed_filters(std::size_t channels) {
    std::vector<std::vector<float>> filters(channels, std::vector<float>(512U));
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t i = 0; i < 512U; ++i) {
            filters[c][i] = static_cast<float>(static_cast<int>((i * 17U + c * 13U) % 31U) - 15) / 512.0F;
        }
    }
    return filters;
}
double time_domain_reference(std::size_t channels) {
    const auto firs = fixed_filters(channels);
    auto bank = take(mradm::dsp::EarFilters::create(channels, flatten(firs), 255U));
    mradm::dsp::EarPostProcessor processor(bank, 1024U);
    constexpr std::size_t k_frames = 1400U;
    std::vector<float> dry(k_frames * channels);
    std::vector<float> wet(dry.size());
    for (std::size_t f = 0; f < 800U; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            dry[(f * channels) + c] = signal(f + c, 3U);
            wet[(f * channels) + c] = signal(f + c, 11U);
        }
    }
    auto actual = dry;
    constexpr std::array<std::size_t, 11> chunks{1U, 7U, 37U, 127U, 254U, 255U, 256U, 510U, 511U, 512U, 1024U};
    std::size_t offset = 0;
    std::size_t index = 0;
    while (offset < k_frames) {
        const auto count = std::min(chunks.at(index++ % chunks.size()), k_frames - offset);
        processor.process(std::span{actual}.subspan(offset * channels, count * channels),
                          std::span{wet}.subspan(offset * channels, count * channels),
                          count);
        offset += count;
    }
    double maximum = 0;
    for (std::size_t f = 0; f < k_frames; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            double value = f >= 255U ? dry[((f - 255U) * channels) + c] : 0.0;
            for (std::size_t tap = 0; tap < std::min(std::size_t{512U}, f + 1U); ++tap) {
                value += static_cast<double>(wet[((f - tap) * channels) + c]) * static_cast<double>(firs[c][tap]);
            }
            const double error = std::abs(static_cast<double>(actual[(f * channels) + c]) - value);
            require(std::isfinite(error) && error <= 2e-5 + (2e-5 * std::abs(value)), "time-domain FIR mismatch");
            maximum = std::max(maximum, error);
        }
    }
    return maximum;
}
} // namespace

int main() {
    try {
        double fir_error = 0;
        for (const std::size_t channels : {2U, 6U, 12U, 24U}) {
            fir_error = std::max(fir_error, time_domain_reference(channels));
        }
        std::cout << "EAR post time-domain max error " << fir_error << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
