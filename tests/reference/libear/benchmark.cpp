// Release-only preparation and steady-state EAR mixing/post-processing measurements.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

#include <ear/ear.hpp>

#include "ear.h"
#include "ear_post.h"
#include "pcm_mix.h"
namespace {
constexpr std::size_t k_blocks = 128;
constexpr std::size_t k_frames = 1024;
struct Prepared {
    std::vector<double> gains;
    std::vector<float> firs;
};
Prepared prepare(bool legacy) {
    constexpr std::size_t k_channels = 24;
    Prepared result;
    result.gains.reserve(k_blocks * k_channels * 2);
    const auto calculate = [&](auto&& run) {
        for (std::size_t i = 0; i < k_blocks; ++i) {
            MradmEarObject meta{
                {-180.0 + static_cast<double>(i) * 360.0 / 127.0, 17.0, 1.0}, 70.0, 20.0, 0.0, 1.0, 0.25};
            std::vector<double> direct(k_channels), diffuse(k_channels);
            run(meta, direct, diffuse);
            result.gains.insert(result.gains.end(), direct.begin(), direct.end());
            result.gains.insert(result.gains.end(), diffuse.begin(), diffuse.end());
        }
    };
    if (legacy) {
        const auto layout = ear::getLayout("9+10+3");
        ear::GainCalculatorObjects objects{layout};
        const ear::GainCalculatorDirectSpeakers direct{layout};
        const ear::GainCalculatorHOA hoa{layout};
        calculate([&](const MradmEarObject& m, auto& a, auto& b) {
            ear::ObjectsTypeMetadata meta;
            meta.position = ear::PolarPosition{m.position[0], m.position[1], m.position[2]};
            meta.width = m.width;
            meta.height = m.height;
            meta.diffuse = m.diffuse;
            objects.calculate(meta, a, b);
        });
        for (const auto& f : ear::designDecorrelators<float>(layout)) {
            result.firs.insert(result.firs.end(), f.begin(), f.end());
        }
    } else {
        const auto layout = mradm::dsp::ear_layout("9+10+3");
        const mradm::dsp::EarCalculator objects{layout};
        calculate([&](const MradmEarObject& m, auto& a, auto& b) { objects.objects(m, a, b); });
        // The production adapter constructs a separate preparation calculator for FIRs.
        result.firs = mradm::dsp::EarCalculator{layout}.filters();
    }
    return result;
}
double steady(const Prepared& prepared) {
    constexpr std::size_t k_channels = 24;
    const std::array rows{MradmDspMixRow{0, 0, k_blocks, 1.0F}};
    std::vector<MradmDspMixBlock> blocks;
    for (std::size_t i = 0; i < k_blocks; ++i) {
        blocks.push_back({i * k_frames, (i + 1) * k_frames, 0, 2U});
    }
    auto plan = mradm::dsp::PcmMixPlan::create(1, k_channels, rows, blocks, {}, prepared.gains, true);
    if (!plan) {
        throw std::runtime_error(plan.error().message);
    }
    auto filters = mradm::dsp::EarFilters::create(k_channels, prepared.firs, 255);
    if (!filters) {
        throw std::runtime_error(filters.error().message);
    }
    mradm::dsp::PcmMixer mix{*plan, k_frames, 0, false};
    mradm::dsp::EarPostProcessor post{*filters, k_frames};
    std::vector<float> input(k_frames), direct(k_frames * k_channels), diffuse(direct.size());
    for (std::size_t i = 0; i < k_frames; ++i) {
        input[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.13) * 0.1);
    }
    const auto start = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < 8; ++repeat) {
        mix.reset();
        post.reset();
        for (std::size_t i = 0; i < k_blocks; ++i) {
            mix.ear(input, direct, diffuse, i * k_frames, k_frames);
            post.process(direct, diffuse, k_frames);
        }
    }
    if (!std::isfinite(direct.back())) {
        throw std::runtime_error("Non-finite benchmark output");
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
} // namespace
int main() try {
    std::array<std::vector<double>, 2> prep, render;
    for (int run = 0; run < 6; ++run) {
        for (int slot = 0; slot < 2; ++slot) {
            const int kind = (slot + run) % 2;
            const auto start = std::chrono::steady_clock::now();
            const auto state = prepare(kind == 0);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const auto process = steady(state);
            if (run > 0) {
                prep.at(static_cast<size_t>(kind)).push_back(elapsed);
                render.at(static_cast<size_t>(kind)).push_back(process);
            }
        }
    }
    const auto median = [](auto values) {
        std::ranges::sort(values);
        return values.at(values.size() / 2);
    };
    std::cout << std::setprecision(17)
              << "{\"layout\":\"22.2\",\"metadata_blocks\":128,\"processed_frames\":1048576,\"sample_rate\":48000,"
                 "\"repetitions\":5,\"legacy_prepare_seconds\":"
              << median(prep[0]) << ",\"rust_prepare_seconds\":" << median(prep[1])
              << ",\"legacy_steady_seconds\":" << median(render[0]) << ",\"rust_steady_seconds\":" << median(render[1])
              << "}\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
