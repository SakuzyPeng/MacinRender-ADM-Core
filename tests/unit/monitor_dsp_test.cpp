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

#include "../reference/monitor/legacy.h"
#include "monitor.h"

namespace {
struct Summary {
    uint64_t samples{0};
    uint64_t different{0};
    double max_error{0};
    double max_ratio{0};
};
void require(bool ok, const char* text) {
    if (!ok) {
        throw std::runtime_error(text);
    }
}
void compare(float actual, float expected, Summary& summary, bool exact = false) {
    ++summary.samples;
    if (std::isnan(expected)) {
        require(std::isnan(actual), "NaN classification differs");
        return;
    }
    if (std::isinf(expected)) {
        require(actual == expected, "infinity classification differs");
        return;
    }
    require(std::isfinite(actual), "unexpected nonfinite result");
    const bool same = std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(expected);
    summary.different += same ? 0U : 1U;
    const double error = std::abs(static_cast<double>(actual) - expected);
    const double tolerance = 2e-6 + (2e-6 * std::abs(static_cast<double>(expected)));
    summary.max_error = std::max(summary.max_error, error);
    summary.max_ratio = std::max(summary.max_ratio, error / tolerance);
    require(exact ? same : error <= tolerance, exact ? "bitwise comparison failed" : "migration tolerance exceeded");
}
std::vector<float> signal(std::size_t samples, std::size_t offset) {
    std::vector<float> result(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        result[i] = static_cast<float>(static_cast<int>(((i * 37) + (offset * 13)) % 1001) - 500) / 509.0F;
    }
    return result;
}
void crossfades(uint32_t channels, Summary& result) {
    monitor_legacy::State before(channels, 48000, true);
    mradm::dsp::MonitorCrossfade original(channels, 2048);
    auto after = std::move(original);
    for (std::size_t iteration = 0; iteration < 50; ++iteration) {
        if (iteration % 7 == 0) {
            before.xfade_pos_ = 0;
            after.reset();
        }
        const auto frames =
            std::array<std::size_t, 10>{0, 1, 7, 37, 511, 512, 1024, 2047, 2048, 4097}.at(iteration % 10);
        auto expected = signal((frames + 1) * channels, iteration);
        auto actual = expected;
        auto incoming = signal(expected.size(), iteration + 9);
        const auto done = before.crossfade(expected, incoming, frames);
        require(after.process(actual, incoming, frames) == done, "fade completion differs");
        for (std::size_t i = 0; i < actual.size(); ++i) {
            compare(actual[i], expected[i], result, i >= frames * channels);
        }
    }
    original = std::move(after);
    original.reset();
    auto exact = std::vector<float>(static_cast<std::size_t>(channels) * 2049U, 0.25F);
    const auto incoming = std::vector<float>(exact.size(), 0.75F);
    require(original.process(exact, incoming, 2049), "exact fade did not complete");
    for (std::size_t f = 0; f < 2049; ++f) {
        for (std::size_t c = 0; c < channels; ++c) {
            compare(exact[(f * channels) + c], 0.25F + (static_cast<float>(f) / 4096.0F), result, true);
        }
    }
}
void callbacks(uint32_t channels, uint32_t rate, bool realtime, Summary& pcm_summary, Summary& metrics) {
    monitor_legacy::State before(channels, rate, realtime);
    mradm::dsp::MonitorOutput original(channels, rate, realtime);
    auto after = std::move(original);
    uint64_t generation = 0;
    std::array<float, 64> p{};
    std::array<float, 64> r{};
    std::array<float, 64> old_p{};
    std::array<float, 64> old_r{};
    constexpr std::array<std::size_t, 11> sizes{1, 0, 7, 37, 255, 479, 480, 511, 1024, 2049, 8193};
    for (std::size_t iteration = 0; iteration < 77; ++iteration) {
        const auto frames = sizes.at(iteration % sizes.size());
        auto produced = frames;
        if (iteration % 9 == 4) {
            produced = frames / 2;
        } else if (iteration % 9 == 5) {
            produced = 0;
        }
        const bool active = iteration % 13 != 6;
        if (iteration % 3 == 1) {
            ++generation;
        }
        if (iteration == 33) {
            generation = std::numeric_limits<uint64_t>::max();
        }
        if (iteration == 55) {
            before = monitor_legacy::State(channels, rate, realtime);
            after.reset();
        }
        auto expected = signal((frames + 1) * channels, iteration);
        if (!active) {
            std::fill_n(expected.begin(), frames * channels, 0.0F);
        } else if (produced < frames) {
            std::fill(expected.begin() + static_cast<std::ptrdiff_t>(produced * channels),
                      expected.begin() + static_cast<std::ptrdiff_t>(frames * channels),
                      0.0F);
        }
        auto actual = expected;
        before.apply_seek_transition(expected, frames, produced, active, generation);
        before.levels(expected, frames, old_p, old_r);
        after.process(actual, frames, produced, active, generation, p, r);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            compare(actual[i], expected[i], pcm_summary, i >= frames * channels);
        }
        for (std::size_t c = 0; c < std::min<std::size_t>(channels, 64); ++c) {
            compare(p.at(c), old_p.at(c), metrics);
            compare(r.at(c), old_r.at(c), metrics);
        }
    }
    original = std::move(after);
}
void identical_input_metrics(uint32_t channels, Summary& metrics) {
    monitor_legacy::State before(channels, 48000, true);
    mradm::dsp::MonitorOutput after(channels, 48000, true);
    std::array<float, 64> p{};
    std::array<float, 64> r{};
    std::array<float, 64> old_p{};
    std::array<float, 64> old_r{};
    for (const auto frames : {0U, 1U, 37U, 511U, 8193U}) {
        auto pcm = signal(static_cast<std::size_t>(frames) * channels, frames);
        for (std::size_t special = 0; special < 4; ++special) {
            if (!pcm.empty() && special > 0) {
                pcm[0] = std::array{0.0F,
                                    std::numeric_limits<float>::quiet_NaN(),
                                    std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity()}
                             .at(special);
                pcm.back() = -0.0F;
            }
            const auto saved = pcm;
            before.levels(pcm, frames, old_p, old_r);
            after.process(pcm, frames, frames, true, 0, p, r);
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                require(std::bit_cast<uint32_t>(pcm[i]) == std::bit_cast<uint32_t>(saved[i]), "meter changed PCM");
            }
            for (std::size_t c = 0; c < std::min<std::size_t>(channels, 64); ++c) {
                compare(p.at(c), old_p.at(c), metrics, true);
                compare(r.at(c), old_r.at(c), metrics, true);
            }
        }
    }
    // Zero-weight multiplication must still propagate NaN, rather than shortcut an endpoint.
    auto pcm = std::vector<float>(channels, 0.25F);
    auto incoming = std::vector<float>(channels, std::numeric_limits<float>::quiet_NaN());
    mradm::dsp::MonitorCrossfade fade(channels, 2048);
    fade.process(pcm, incoming, 1);
    require(std::ranges::all_of(pcm, [](float v) { return std::isnan(v); }), "zero-weight NaN was sanitized");
}
void json_summary(std::ostream& out, const Summary& s) {
    out << "{\"samples\":" << s.samples << ",\"differing_samples\":" << s.different
        << ",\"max_absolute_error\":" << s.max_error << ",\"max_tolerance_ratio\":" << s.max_ratio << '}';
}
} // namespace
int main(int argc, char** argv) {
    try {
        Summary fade;
        Summary pcm;
        Summary metrics;
        Summary identical;
        for (uint32_t channels : {1U, 2U, 12U, 64U, 96U}) {
            crossfades(channels, fade);
            identical_input_metrics(channels, identical);
            for (uint32_t rate : {1U, 1000U, 8000U, 44100U, 48000U, 96000U, 192000U}) {
                for (bool realtime : {false, true}) {
                    callbacks(channels, rate, realtime, pcm, metrics);
                }
            }
        }
        if (argc > 1) {
            std::ofstream out(argv[1]);
            require(out.good(), "cannot open numerical report");
            out << std::setprecision(17) << R"({"baseline":"a54fdc3","passed":true,"crossfade":)";
            json_summary(out, fade);
            out << ",\"seek_pcm\":";
            json_summary(out, pcm);
            out << ",\"combined_levels\":";
            json_summary(out, metrics);
            out << ",\"identical_input_levels\":";
            json_summary(out, identical);
            out << "}\n";
        }
        std::cout << "Monitor comparison passed: " << fade.samples + pcm.samples + metrics.samples + identical.samples
                  << " values\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
