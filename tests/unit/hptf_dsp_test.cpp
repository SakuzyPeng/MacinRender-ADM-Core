// Independent transition/partition contracts and an optional Release migration
// capture. Capture rows are numeric text, avoiding a third-party test dependency.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "hptf_eq.h"

namespace {
using namespace mradm::render_common;
using mradm::HptfPreampMode;

void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

class Capture {
  public:
    explicit Capture(const char* path) {
        if (path != nullptr) {
            file_.open(path);
            require(file_.is_open(), "open HpTF capture");
            file_ << std::setprecision(17);
        }
    }
    void row(std::string_view kind, std::string_view name, std::span<const double> values) {
        if (file_.is_open()) {
            file_ << kind << ' ' << name << ' ' << values.size();
            for (double value : values) {
                require(std::isfinite(value), "finite capture value");
                file_ << ' ' << value;
            }
            file_ << '\n';
            require(file_.good(), "write HpTF capture");
        }
    }
    void pcm(std::string_view name, std::span<const float> values) {
        const std::vector<double> widened(values.begin(), values.end());
        row("pcm", name, widened);
    }

  private:
    std::ofstream file_;
};

HptfCoefficients gain(float value) {
    HptfCoefficients result;
    result.sample_rate = 48000;
    result.preamp_gain = value;
    result.preamp_db = 20.0F * std::log10(value);
    result.max_response_db = result.preamp_db;
    return result;
}

void transitions(Capture& capture) {
    HptfProcessor processor;
    processor.prepare(2, 48000);
    processor.publish(gain(0.5F), 1);
    processor.process(nullptr, 0);
    require(processor.applied_revision() == 0, "empty processing keeps pending update");
    std::vector<float> pcm(4096, 1.0F);
    processor.process(pcm.data(), 2048);
    for (std::size_t frame = 0; frame < 2048U; ++frame) {
        const float expected = 1.0F - (0.5F * static_cast<float>(frame) / 2048.0F);
        require(pcm[frame * 2U] == expected && pcm[(frame * 2U) + 1U] == expected,
                "linear fade starts at the old arm and preserves channel mapping");
    }
    capture.pcm("initial-fade", pcm);
    require(processor.applied_revision() == 1, "revision applies at fade completion");
    processor.publish(gain(0.25F), 2);
    pcm.assign(514, 1.0F);
    processor.process(pcm.data(), 257);
    capture.pcm("partial-fade", pcm);
    processor.publish(gain(0.75F), 3);
    pcm.assign(4800, 1.0F);
    processor.process(pcm.data(), 2400);
    require(processor.applied_revision() == 2, "pending target waits for the next process call");
    require(pcm.back() == 0.25F, "large blocks finish the current fade without a third arm");
    capture.pcm("finish-with-pending", pcm);
    pcm.assign(274, 1.0F);
    processor.process(pcm.data(), 137);
    require(pcm.front() == 0.25F && processor.applied_revision() == 2, "queued target starts at next call");
    capture.pcm("queued-fade", pcm);
    processor.reset_state();
    require(processor.applied_revision() == 3, "seek adopts the active fade target");
    processor.publish(gain(0.125F), 4);
    processor.reset_state();
    pcm.assign(256, 1.0F);
    processor.process(pcm.data(), 128);
    require(std::ranges::all_of(pcm, [](float x) { return x == 0.125F; }), "seek adopts pending target immediately");
    capture.pcm("seek-pending", pcm);
    processor.publish_bypass(5);
    processor.reset_state();
    require(processor.applied_revision() == 5 && processor.active_coefficients().is_bypass(), "seek adopts bypass");
}

std::vector<float> signal(std::size_t frames) {
    std::vector<float> result(frames * 2U);
    std::uint32_t random = 7;
    for (float& sample : result) {
        random = (random * 1664525U) + 1013904223U;
        sample = (static_cast<float>(random >> 8U) / 16777216.0F - 0.5F) * 0.25F;
    }
    return result;
}

void design_and_partition(Capture& capture) {
    constexpr std::array types{HptfBandType::peaking,
                               HptfBandType::low_shelf,
                               HptfBandType::high_shelf,
                               HptfBandType::low_pass,
                               HptfBandType::high_pass,
                               HptfBandType::band_pass,
                               HptfBandType::notch};
    for (const std::uint32_t rate : {8000U, 32000U, 44100U, 48000U, 96000U, 192000U}) {
        for (std::size_t type = 0; type < types.size(); ++type) {
            for (const auto mode : {HptfPreampMode::warn_only, HptfPreampMode::auto_trim}) {
                HptfProfile profile;
                profile.preamp_db = -1.5;
                profile.bands.push_back({types.at(type), true, 997.13, 9.0, 2.5});
                const auto result = design_cascade(profile, rate, mode);
                require(result.has_value(), "design filter at every supported fixture rate");
                const auto& c = *result;
                const std::string name = std::to_string(rate) + "-" + std::to_string(type) + "-" +
                                         (mode == HptfPreampMode::auto_trim ? "trim" : "warn");
                const auto& s = c.sections.front();
                const std::array<double, 6> coefficients{c.preamp_gain, s.b0, s.b1, s.b2, s.a1, s.a2};
                capture.row("coefficients", name, coefficients);
                std::vector<double> response{c.max_response_db, c.auto_trim_db};
                for (double frequency : {20.0, 37.13, 105.0, 997.13, 1013.0, 3100.0, 17000.0}) {
                    response.push_back(cascade_magnitude_db(c, std::min(frequency, static_cast<double>(rate) * 0.49)));
                }
                capture.row("response_db", name, response);
                auto whole = signal(4096);
                auto partitioned = whole;
                HptfCascade first;
                HptfCascade second;
                first.prepare(2);
                second.prepare(2);
                first.set_coefficients(c);
                second.set_coefficients(c);
                first.process(whole.data(), 4096);
                for (std::size_t offset = 0; offset < 4096U;) {
                    const auto count = std::min(std::size_t{137U}, 4096U - offset);
                    second.process(partitioned.data() + (offset * 2U), count);
                    offset += count;
                }
                require(whole == partitioned, "cascade history is independent of active-signal partitioning");
                capture.pcm(name, whole);
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc <= 2, "optional argument is a Release capture path");
        Capture capture(argc == 2 ? argv[1] : nullptr);
        transitions(capture);
        design_and_partition(capture);
        std::cout << "HpTF DSP transitions and 84 design/partition cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
