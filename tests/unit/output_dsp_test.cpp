#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../reference/output_dsp_legacy.h"
#include "render_common.h"
#include "stereo_peak_guard.h"

namespace {
struct Comparison {
    std::uint64_t gain_samples{0};
    std::uint64_t peak_samples{0};
    double peak_max_error{0};
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void same_gain(float actual, float expected, Comparison& result) {
    require(std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected), "gain bit mismatch");
    ++result.gain_samples;
}

void compare_multichannel_gains(std::uint32_t rate, std::span<const std::size_t> lengths, Comparison& result) {
    for (const std::size_t channels : {1U, 2U, 12U, 64U}) {
        output_dsp_legacy::InterleavedLiveGainSmoother before(channels, rate);
        mradm::render_common::InterleavedLiveGainSmoother after(channels, rate);
        for (const auto target_count : {channels, std::size_t{1}, std::size_t{0}, channels + 2}) {
            std::vector<float> gains(target_count, 0.125F);
            before.set_targets(gains);
            after.set_targets(gains);
            for (const auto frames : lengths) {
                std::vector<float> expected(frames * channels);
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    expected[i] = static_cast<float>(static_cast<int>(i % 31) - 15) / 16.0F;
                }
                auto actual = expected;
                before.apply(expected.data(), frames);
                after.apply(actual.data(), frames);
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    same_gain(actual[i], expected[i], result);
                }
            }
        }
        before = output_dsp_legacy::InterleavedLiveGainSmoother(channels, rate);
        after.reset();
        const std::array<float, 1> initial{0.75F};
        before.set_targets(initial);
        after.set_targets(initial);
        std::vector<float> expected(channels * 7U, 1.0F);
        auto actual = expected;
        before.apply(expected.data(), 7U);
        after.fill(actual);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            same_gain(actual[i], expected[i], result);
        }
    }
}

void compare_gains(Comparison& result) {
    constexpr std::array<std::uint32_t, 6> rates{1000U, 8000U, 44100U, 48000U, 96000U, 192000U};
    constexpr std::array<float, 7> targets{0.25F, 1.0F, 0.0F, 0.0F, 0.5F, 2.0F, 0.125F};
    constexpr std::array<std::size_t, 8> lengths{0U, 1U, 7U, 37U, 511U, 1024U, 1U, 4096U};
    for (const auto rate : rates) {
        for (const auto ramp_ms : {0U, 4U, 20U}) {
            output_dsp_legacy::LiveGainRamp before(rate, ramp_ms);
            mradm::render_common::LiveGainRamp after(rate, ramp_ms);
            for (float target : targets) {
                before.set_target(target);
                after.set_target(target);
                for (const auto length : lengths) {
                    std::vector<float> actual(length);
                    after.fill(actual);
                    for (const auto value : actual) {
                        same_gain(value, before.next(), result);
                    }
                }
            }
            // Interrupt ramps at fixed sample positions, including repeated and pre-first-sample targets.
            for (std::size_t edit = 0; edit < 80U; ++edit) {
                const float target = targets.at((edit / 3U) % targets.size());
                before.set_target(target);
                after.set_target(target);
                std::vector<float> actual(lengths.at(edit % 5U));
                after.fill(actual);
                for (const auto value : actual) {
                    same_gain(value, before.next(), result);
                }
            }
            auto moved = std::move(after);
            same_gain(moved.next(), before.next(), result);
            after = std::move(moved);
            same_gain(after.next(), before.next(), result);
        }
        compare_multichannel_gains(rate, lengths, result);
    }
}

void compare_peak(std::uint32_t rate, std::size_t chunk, Comparison& result) {
    output_dsp_legacy::StereoPeakGuard before(rate);
    mradm::realtime::StereoPeakGuard original(rate);
    auto after = std::move(original);
    require(before.lookahead_frames() == after.lookahead_frames(), "lookahead mismatch");
    std::vector<float> input(std::size_t{11003U} * 2U);
    for (std::size_t i = 0; i < input.size() / 2U; ++i) {
        input[i * 2U] = i % 701U == 0U ? 8.0F : 0.4F * std::sin(static_cast<float>(i) * 0.013F);
        input[(i * 2U) + 1U] = input[i * 2U] * -0.25F;
    }
    input[124U] = std::numeric_limits<float>::infinity();
    input[125U] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> expected(1022U);
    std::vector<float> actual(expected.size());
    std::size_t read = 0U;
    std::size_t written = 0U;
    std::size_t iteration = 0U;
    while (read < input.size() || after.buffered_frames() != 0U) {
        require(before.writable_frames() == after.writable_frames(), "writable mismatch");
        const auto take = std::min({chunk, (input.size() - read) / 2U, after.writable_frames()});
        const auto piece = std::span{input}.subspan(read, take * 2U);
        before.push(piece);
        after.push(piece);
        read += take * 2U;
        const bool ended = read == input.size();
        require(before.readable_frames(ended) == after.readable_frames(ended), "readable mismatch");
        const float volume = std::array{0.0F, 0.1F, 0.8F, 1.0F}.at((iteration++ / 7U) % 4U);
        std::ranges::fill(expected, 42.0F);
        std::ranges::fill(actual, 42.0F);
        const std::size_t want = iteration % 2U == 0U ? 1U : 511U;
        const auto old_count = before.pop(std::span{expected}.first(want * 2U), volume, ended);
        const auto count = after.pop(std::span{actual}.first(want * 2U), volume, ended);
        require(old_count == count && (take != 0U || count != 0U), "peak output length/progress mismatch");
        for (std::size_t i = 0; i < count * 2U; ++i) {
            require(std::isfinite(actual[i]), "nonfinite protected PCM");
            const double difference = std::abs(static_cast<double>(actual[i]) - expected[i]);
            result.peak_max_error = std::max(result.peak_max_error, difference);
            require(difference <= 2.0e-5, "peak PCM exceeds migration tolerance");
            require(std::abs(actual[i]) <= mradm::realtime::StereoPeakGuard::k_ceiling + 1.0e-6F,
                    "peak exceeds ceiling");
            ++result.peak_samples;
        }
        require(std::ranges::all_of(std::span{actual}.subspan(count * 2U), [](float value) { return value == 42.0F; }),
                "pop overwrote unproduced frames");
        written += count;
    }
    require(written * 2U == input.size(), "peak guard changed media duration");
    after.reset();
    const std::array<float, 4> quiet{0.125F, -0.25F, 0.5F, -0.75F};
    after.push(quiet);
    original = std::move(after);
    require(original.pop(actual, 0.5F, true) == 2U, "reset/move lost buffered frames");
    for (std::size_t i = 0; i < quiet.size(); ++i) {
        require(actual[i] == quiet.at(i) * 0.5F, "transparent PCM changed");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        Comparison result;
        compare_gains(result);
        for (const auto rate : {8000U, 44100U, 48000U, 96000U, 192000U}) {
            for (const std::size_t chunk : {1U, 37U, 512U, 4096U}) {
                compare_peak(rate, chunk, result);
            }
        }
        const auto report = [&](std::ostream& out) {
            out << std::setprecision(17)
                << "{\n  \"baseline_commit\": \"53cc36e\",\n  \"gain_samples\": " << result.gain_samples
                << ",\n  \"gain_bit_mismatches\": 0,\n  \"peak_samples\": " << result.peak_samples
                << ",\n  \"peak_max_absolute_error\": " << result.peak_max_error
                << ",\n  \"peak_tolerance\": 0.00002\n}\n";
        };
        report(std::cout);
        if (argc == 2) {
            std::ofstream output(argv[1]);
            report(output);
            require(output.good(), "cannot write comparison report");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
