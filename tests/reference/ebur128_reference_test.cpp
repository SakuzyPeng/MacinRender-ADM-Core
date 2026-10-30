// Optional Release reference for the complete C++ -> private FFI -> Rust path.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ebur128.h>
#include <iostream>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "meter.h"
#include "post_gain.h"

namespace {
using mradm::dsp::Meter;
using mradm::dsp::MeterChannel;
using mradm::dsp::MeterMode;
struct ReferenceFree {
    void operator()(ebur128_state* state) const noexcept { ebur128_destroy(&state); }
};

bool near(double a, double b, double tolerance) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < tolerance;
}

bool compare(uint32_t sample_rate, uint32_t channels, bool monitor, bool explicit_map) {
    constexpr std::array rust_map{MeterChannel::left,
                                  MeterChannel::right,
                                  MeterChannel::center,
                                  MeterChannel::unused,
                                  MeterChannel::side_left,
                                  MeterChannel::side_right,
                                  MeterChannel::rear_left,
                                  MeterChannel::rear_right,
                                  MeterChannel::top_front_left,
                                  MeterChannel::top_front_right,
                                  MeterChannel::top_rear_left,
                                  MeterChannel::top_rear_right};
    constexpr std::array c_map{EBUR128_Mp030,
                               EBUR128_Mm030,
                               EBUR128_Mp000,
                               EBUR128_UNUSED,
                               EBUR128_Mp090,
                               EBUR128_Mm090,
                               EBUR128_Mp135,
                               EBUR128_Mm135,
                               EBUR128_Up045,
                               EBUR128_Um045,
                               EBUR128_Up135,
                               EBUR128_Um135};
    const std::span<const MeterChannel> mapping =
        explicit_map ? std::span<const MeterChannel>{rust_map} : std::span<const MeterChannel>{};
    auto rust =
        Meter::create(channels, sample_rate, monitor ? MeterMode::monitor : MeterMode::integrated_true_peak, mapping);
    std::unique_ptr<ebur128_state, ReferenceFree> reference{ebur128_init(
        channels, sample_rate, monitor ? EBUR128_MODE_S | EBUR128_MODE_I : EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};
    if (!rust || !reference) {
        return false;
    }
    if (explicit_map) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            if (ebur128_set_channel(reference.get(), ch, c_map.at(ch)) != EBUR128_SUCCESS) {
                return false;
            }
        }
    }
    std::vector<float> block(512U * channels);
    const uint64_t total = static_cast<uint64_t>(sample_rate) * 4U + 137U;
    for (uint64_t base = 0; base < total; base += 512U) {
        const auto frames = static_cast<std::size_t>(std::min<uint64_t>(512U, total - base));
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const double time = static_cast<double>(base + frame) / sample_rate;
                block[(frame * channels) + ch] =
                    static_cast<float>(0.17 * std::sin(2.0 * std::numbers::pi * (440.0 + (113.0 * ch)) * time));
            }
        }
        if (!rust->add_frames(block.data(), frames) ||
            ebur128_add_frames_float(reference.get(), block.data(), frames) != EBUR128_SUCCESS) {
            return false;
        }
    }
    double c_lufs = 0.0;
    if (ebur128_loudness_global(reference.get(), &c_lufs) != EBUR128_SUCCESS ||
        !near(rust->integrated().value_or(0.0), c_lufs, 0.001)) {
        return false;
    }
    if (monitor) {
        double momentary = 0.0;
        double shortterm = 0.0;
        return ebur128_loudness_momentary(reference.get(), &momentary) == EBUR128_SUCCESS &&
               ebur128_loudness_shortterm(reference.get(), &shortterm) == EBUR128_SUCCESS &&
               near(rust->momentary().value_or(0.0), momentary, 0.001) &&
               near(rust->shortterm().value_or(0.0), shortterm, 0.001);
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
        double peak = 0.0;
        if (ebur128_true_peak(reference.get(), channel, &peak) != EBUR128_SUCCESS ||
            !near(20.0 * std::log10(rust->true_peak(channel).value_or(0.0)), 20.0 * std::log10(peak), 0.01)) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<ebur128_state, ReferenceFree> measure_block(const std::vector<float>& block, uint32_t channels) {
    std::unique_ptr<ebur128_state, ReferenceFree> reference{
        ebur128_init(channels, 48000U, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};
    if (!reference ||
        ebur128_add_frames_float(reference.get(), block.data(), block.size() / channels) != EBUR128_SUCCESS) {
        return {};
    }
    return reference;
}

// Rust meter -> post_gain decision -> gain applied in place, the offline post-processing chain.
bool apply_measured_gain(std::vector<float>& block, uint32_t channels, bool loudness, double target) {
    auto meter = Meter::create(channels, 48000U, loudness ? MeterMode::integrated : MeterMode::true_peak);
    if (!meter || !meter->add_frames(block.data(), block.size() / channels)) {
        return false;
    }
    const auto measured = loudness ? meter->integrated() : meter->max_true_peak();
    if (!measured) {
        return false;
    }
    const auto gain =
        loudness
            ? mradm::dsp::loudness_gain(*measured, target).map([](auto g) { return std::pair{g.apply, g.gain}; })
            : mradm::dsp::peak_limit_gain(*measured, target).map([](auto g) { return std::pair{g.apply, g.gain}; });
    return gain && gain->first && mradm::dsp::apply_gain(block, gain->second);
}

// The offline post-processing gains checked against libebur128.
bool verify_postprocessing_gain() {
    constexpr uint32_t channels = 2U;
    std::vector<float> block(48000U * 4U * channels);
    for (std::size_t frame = 0; frame < block.size() / channels; ++frame) {
        const auto sample =
            static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(frame) / 48000.0));
        block[frame * channels] = sample;
        block[(frame * channels) + 1U] = sample;
    }
    if (!apply_measured_gain(block, channels, true, -23.0)) {
        return false;
    }
    auto normalized = measure_block(block, channels);
    double lufs = 0.0;
    if (!normalized || ebur128_loudness_global(normalized.get(), &lufs) != EBUR128_SUCCESS ||
        !near(lufs, -23.0, 0.01)) {
        return false;
    }
    if (!apply_measured_gain(block, channels, false, -35.0)) {
        return false;
    }
    auto limited = measure_block(block, channels);
    if (!limited) {
        return false;
    }
    for (unsigned int channel = 0; channel < channels; ++channel) {
        double value = 0.0;
        if (ebur128_true_peak(limited.get(), channel, &value) != EBUR128_SUCCESS ||
            !near(20.0 * std::log10(value), -35.0, 0.001)) {
            return false;
        }
    }
    std::cout << "Post-processing gain matches C reference: " << lufs << " LUFS, -35 dBTP\n";
    return true;
}
} // namespace

int main() {
    for (uint32_t rate : {44100U, 48000U, 96000U, 192000U}) {
        for (uint32_t channels : {1U, 2U, 4U, 5U, 6U, 12U}) {
            for (bool monitor : {false, true}) {
                if (!compare(rate, channels, monitor, channels == 12U)) {
                    std::cerr << "Meter reference mismatch: " << rate << " Hz, " << channels
                              << " channels, monitor=" << monitor << '\n';
                    return EXIT_FAILURE;
                }
            }
        }
    }
    std::cout << "48 C++/FFI/Rust meter comparisons passed\n";
    return verify_postprocessing_gain() ? EXIT_SUCCESS : EXIT_FAILURE;
}
