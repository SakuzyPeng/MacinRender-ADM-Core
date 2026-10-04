#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "meter.h"

namespace {
using mradm::dsp::Meter;
using mradm::dsp::MeterChannel;
using mradm::dsp::MeterMode;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool verify_owner_and_errors() {
    auto invalid = Meter::create(0U, 48000U, MeterMode::monitor);
    if (!check(!invalid && invalid.error().code == mradm::ErrorCode::invalid_argument,
               "invalid dimensions retain their error classification")) {
        return false;
    }
    auto made = Meter::create(2U, 48000U, MeterMode::monitor);
    if (!check(static_cast<bool>(made), "create monitor meter")) {
        return false;
    }
    Meter owner = std::move(*made);
    bool ok = check(!made->reset(), "moved-from handle is empty");
    ok &= check(static_cast<bool>(owner.add_frames(nullptr, 0U)), "empty input is valid");
    ok &= check(!owner.add_frames(nullptr, 1U), "nonempty null input is rejected");
    ok &= check(!owner.add_frames(nullptr, std::numeric_limits<std::size_t>::max()), "frame overflow is rejected");
    ok &= check(!owner.true_peak(0U), "queries must match the configured mode");
    constexpr std::array bad_samples{0.0F, std::numeric_limits<float>::infinity()};
    ok &= check(!owner.add_frames(bad_samples.data(), 1U), "non-finite PCM is rejected");
    const auto loudness = owner.integrated();
    ok &= check(loudness && *loudness == -std::numeric_limits<double>::infinity(), "failed feed leaves state intact");
    return ok;
}

bool verify_measurement_and_reset() {
    constexpr uint32_t k_sample_rate = 48000U;
    std::vector<float> input(static_cast<std::size_t>(k_sample_rate) * 4U);
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
        input[frame] = static_cast<float>(
            0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(frame) / k_sample_rate));
    }
    auto meter = Meter::create(1U, k_sample_rate, MeterMode::integrated_true_peak);
    if (!meter || !meter->add_frames(input.data(), input.size())) {
        return check(false, "feed mono tone");
    }
    const auto loudness = meter->integrated();
    const auto peak = meter->max_true_peak();
    bool ok = check(loudness && std::abs(*loudness + 23.0) < 0.02, "independent -23 LUFS tone");
    ok &= check(peak && std::abs((20.0 * std::log10(*peak)) + 20.0) < 0.1, "independent -20 dBTP peak");
    ok &= check(!meter->true_peak(1U), "channel query is checked");
    if (!meter->reset()) {
        return check(false, "reset meter");
    }
    const auto silence = meter->integrated();
    ok &= check(silence && !std::isfinite(*silence), "seek resets integrated loudness");
    ok &= check(meter->max_true_peak().value_or(-1.0) == 0.0, "seek resets peak history");
    for (std::size_t offset = 0; offset < input.size(); offset += 127U) {
        if (!meter->add_frames(input.data() + offset, std::min<std::size_t>(127U, input.size() - offset))) {
            return check(false, "chunked feed");
        }
    }
    ok &= check(loudness && std::abs(meter->integrated().value_or(0.0) - *loudness) < 1e-9,
                "chunk size and reused state preserve measurements");
    return ok;
}

bool verify_channel_map() {
    constexpr std::array map{MeterChannel::left,
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
    auto meter = Meter::create(12U, 48000U, MeterMode::integrated_true_peak, map);
    std::vector<float> input(std::size_t{48000U} * 12U, 0.0F);
    for (std::size_t frame = 0; frame < 48000U; ++frame) {
        input[(frame * 12U) + 3U] =
            static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(frame) / 48000.0));
    }
    if (!meter || !meter->add_frames(input.data(), 48000U)) {
        return check(false, "feed explicit 7.1.4 LFE");
    }
    bool ok =
        check(meter->integrated().value_or(0.0) == -std::numeric_limits<double>::infinity(), "LFE excluded from LUFS");
    ok &= check(meter->true_peak(3U).value_or(0.0) > 0.19, "LFE retains peak measurement");
    if (!meter->reset()) {
        return false;
    }
    for (std::size_t frame = 0; frame < 48000U; ++frame) {
        std::swap(input[(frame * 12U) + 3U], input[(frame * 12U) + 11U]);
    }
    if (!meter->add_frames(input.data(), 48000U)) {
        return false;
    }
    ok &= check(std::isfinite(meter->integrated().value_or(-std::numeric_limits<double>::infinity())),
                "top rear channel remains mapped after reset");
    return ok;
}
} // namespace

int main() {
    const bool owners = verify_owner_and_errors();
    const bool measurement = verify_measurement_and_reset();
    const bool mapping = verify_channel_map();
    return owners && measurement && mapping ? EXIT_SUCCESS : EXIT_FAILURE;
}
