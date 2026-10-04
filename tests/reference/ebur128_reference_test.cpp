// Optional Release reference for the complete C++ -> private FFI -> Rust path.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ebur128.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <vector>

#include "adm/audio_io.h"
#include "adm/logging.h"
#include "adm/loudness.h"
#include "adm/peak.h"

#include "meter.h"

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

std::unique_ptr<ebur128_state, ReferenceFree> measure_file(const std::string& path) {
    auto reader = mradm::audio::FloatWavReader::open(path);
    if (!reader) {
        return {};
    }
    std::unique_ptr<ebur128_state, ReferenceFree> reference{
        ebur128_init(reader->channels(), reader->sample_rate(), EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};
    if (!reference) {
        return {};
    }
    std::vector<float> block(4096U * reader->channels());
    uint64_t remaining = reader->frame_count();
    while (remaining != 0U) {
        const auto frames = reader->read(block.data(), std::min<uint64_t>(remaining, 4096U));
        if (frames == 0U || ebur128_add_frames_float(reference.get(), block.data(), frames) != EBUR128_SUCCESS) {
            return {};
        }
        remaining -= frames;
    }
    return reference;
}

bool verify_file_postprocessing() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() / ("mradm_meter_reference_" + std::to_string(stamp) + ".wav");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    {
        auto writer = mradm::audio::FloatWavWriter::open(path.string(), 2U, 48000U);
        if (!writer) {
            return false;
        }
        std::vector<float> block(48000U * 2U);
        for (std::size_t frame = 0; frame < 48000U; ++frame) {
            const auto sample = static_cast<float>(
                0.8 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(frame) / 48000.0));
            block[frame * 2U] = sample;
            block[(frame * 2U) + 1U] = sample;
        }
        for (int second = 0; second < 4; ++second) {
            if (writer->write(block.data(), 48000U) != 48000U) {
                return false;
            }
        }
    }
    mradm::NullLogSink logs;
    if (!mradm::apply_loudness_norm(path.string(), -23.0F, logs)) {
        return false;
    }
    auto normalized = measure_file(path.string());
    double lufs = 0.0;
    if (!normalized || ebur128_loudness_global(normalized.get(), &lufs) != EBUR128_SUCCESS ||
        !near(lufs, -23.0, 0.01)) {
        return false;
    }
    if (!mradm::apply_peak_limit(path.string(), -35.0F, logs)) {
        return false;
    }
    auto limited = measure_file(path.string());
    if (!limited) {
        return false;
    }
    for (unsigned int channel = 0; channel < 2U; ++channel) {
        double peak = 0.0;
        if (ebur128_true_peak(limited.get(), channel, &peak) != EBUR128_SUCCESS ||
            !near(20.0 * std::log10(peak), -35.0, 0.001)) {
            return false;
        }
    }
    std::cout << "File postprocessing matches C reference: " << lufs << " LUFS, -35 dBTP\n";
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
    return verify_file_postprocessing() ? EXIT_SUCCESS : EXIT_FAILURE;
}
