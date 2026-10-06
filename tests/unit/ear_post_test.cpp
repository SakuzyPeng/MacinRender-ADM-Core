#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../reference/ear_post_legacy.h"
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
struct Stats {
    std::uint64_t samples{};
    std::uint64_t differing{};
    double absolute{};
    double normalized{};
};
void compare(Stats& stats, std::span<const float> actual, std::span<const float> reference) {
    require(actual.size() == reference.size(), "sample count changed");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const double error = std::abs(static_cast<double>(actual[i]) - reference[i]);
        const double scaled = error / (1.0 + std::abs(static_cast<double>(reference[i])));
        require(std::isfinite(error) && scaled <= 2e-6, "legacy comparison exceeded tolerance");
        ++stats.samples;
        stats.differing +=
            std::bit_cast<std::uint32_t>(actual[i]) != std::bit_cast<std::uint32_t>(reference[i]) ? 1U : 0U;
        stats.absolute = std::max(stats.absolute, error);
        stats.normalized = std::max(stats.normalized, scaled);
    }
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

void legacy_compatibility(std::size_t channels, std::size_t capacity, std::size_t tail, Stats& stats) {
    auto filters = take(mradm::dsp::EarFilters::create(channels, flatten(fixed_filters(channels)), 255U));
    mradm::dsp::EarPostProcessor actual(filters, capacity);
    ear_post_legacy::DecorrState legacy;
    ear_post_legacy::init_decorr_state(legacy, fixed_filters(channels), static_cast<std::uint16_t>(channels), capacity);
    std::size_t start = 0;
    for (const auto frames : {capacity, capacity, tail}) {
        std::vector<float> direct(frames * channels);
        std::vector<float> diffuse(direct.size());
        for (std::size_t i = 0; i < direct.size(); ++i) {
            direct[i] = signal((start * channels) + i, 3U);
            diffuse[i] = signal((start * channels) + i, 11U);
        }
        auto reference = direct;
        std::vector<float> filtered(direct.size());
        ear_post_legacy::apply_decorrelator(legacy, diffuse, filtered, frames, channels);
        ear_post_legacy::apply_direct_delay(legacy, reference, frames, channels);
        for (std::size_t i = 0; i < reference.size(); ++i) {
            reference[i] += filtered[i];
        }
        actual.process(direct, diffuse, frames);
        compare(stats, direct, reference);
        start += frames;
    }
    actual.reset();
    mradm::dsp::EarPostProcessor fresh(filters, capacity);
    filters = {}; // processors own the shared Rust bank independently of this C++ handle
    std::vector<float> direct(tail * channels, 0.25F);
    const std::vector<float> diffuse(direct.size(), 0.125F);
    auto reference = direct;
    actual.process(direct, diffuse, tail);
    fresh.process(reference, diffuse, tail);
    require(direct == reference, "reset differs from fresh state");
    auto moved = std::move(actual);
    std::ranges::fill(direct, 0.25F);
    reference = direct;
    moved.process(direct, diffuse, tail);
    fresh.process(reference, diffuse, tail);
    require(direct == reference, "moving a post processor lost history");
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

std::pair<std::array<float, 3>, std::array<float, 3>> short_tail_regression() {
    constexpr std::size_t channels = 2U;
    ear_post_legacy::DecorrState legacy;
    ear_post_legacy::init_decorr_state(legacy, fixed_filters(channels), 2U, 1U);
    std::vector<float> impulse(legacy.fft_len, 0.0F);
    impulse[0] = 1;
    impulse[1] = 0.5F;
    impulse[2] = 0.25F;
    for (auto& spectrum : legacy.filter_fd) {
        ear_post_legacy::dsp::fft_forward(legacy.hFFT, impulse.data(), spectrum.data());
    }
    std::vector<float> firs(1024U, 0.0F);
    for (std::size_t ch = 0; ch < 2U; ++ch) {
        std::copy_n(impulse.begin(), 512U, firs.begin() + static_cast<std::ptrdiff_t>(ch * 512U));
    }
    auto bank = take(mradm::dsp::EarFilters::create(2U, firs, 255U));
    mradm::dsp::EarPostProcessor processor(bank, 1U);
    std::array<float, 3> before{};
    std::array<float, 3> after{};
    for (std::size_t i = 0; i < 3U; ++i) {
        std::vector<float> wet(2U, i == 0U ? 1.0F : 0.0F);
        std::vector<float> filtered(2U);
        ear_post_legacy::apply_decorrelator(legacy, wet, filtered, 1U, 2U);
        std::array<float, 2> direct{};
        processor.process(direct, wet, 1U);
        before.at(i) = filtered[0];
        after.at(i) = direct[0];
    }
    require(std::abs(before.at(2)) < 1e-6F && std::abs(after.at(2) - 0.25F) < 1e-6F,
            "short-block regression was not exercised");
    return {before, after};
}
} // namespace

int main(int argc, char** argv) {
    try {
        Stats stats;
        double fir_error = 0;
        for (const std::size_t channels : {2U, 6U, 12U, 24U}) {
            for (const std::size_t capacity : {1024U, 1536U, 2048U}) {
                for (const std::size_t tail : {1U, 7U, 200U, 254U, 255U, 256U, 510U, 511U, 512U, 1023U}) {
                    legacy_compatibility(channels, capacity, tail, stats);
                }
            }
            fir_error = std::max(fir_error, time_domain_reference(channels));
        }
        const auto [before, after] = short_tail_regression();
        const auto report = [&](std::ostream& out) {
            out << std::setprecision(17) << R"json({"baseline_commit":"e12332b","canonical_samples":)json"
                << stats.samples << R"json(,"differing_samples":)json" << stats.differing
                << R"json(,"max_absolute_error":)json" << stats.absolute << R"json(,"max_normalized_error":)json"
                << stats.normalized << R"json(,"time_domain_max_error":)json" << fir_error
                << R"json(,"legacy_short_tail":[)json" << before.at(0) << ',' << before.at(1) << ',' << before.at(2)
                << R"json(],"fixed_short_tail":[)json" << after.at(0) << ',' << after.at(1) << ',' << after.at(2)
                << "]}\n";
        };
        report(std::cout);
        if (argc == 2) {
            std::ofstream file(argv[1]);
            report(file);
            require(file.good(), "cannot write comparison report");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
