#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include "size_processor.h"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

std::vector<float> read_floats(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto bytes = input.tellg();
    if (bytes <= 0) {
        return {};
    }
    std::vector<float> data(static_cast<std::size_t>(bytes) / sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), bytes);
    return data;
}

std::vector<float>
render(mradm::triple_balance::SizeObjectProcessor& processor, std::span<const float> input, std::size_t chunk) {
    std::vector<float> output;
    for (std::size_t start = 0; start < input.size(); start += chunk) {
        if (!processor.push(input.subspan(start, std::min(chunk, input.size() - start)), output)) {
            return {};
        }
    }
    if (!processor.finish(output)) {
        return {};
    }
    return output;
}
} // namespace

// NOLINTNEXTLINE(readability-function-size): standalone golden/state regression runner.
int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path fixtures(argv[1]);
    std::ifstream reference_file(fixtures / "reference.json");
    const auto reference = nlohmann::json::parse(reference_file);
    bool ok = true;
    for (const auto& item : reference.at("gains")) {
        const auto& p = item.at("parameters");
        const mradm::triple_balance::QuantizedSizeParameters parameters{{p[0], p[1], p[2]}, p[3]};
        const auto actual = mradm::triple_balance::raw_size_gains(parameters);
        ok &= require(actual.has_value(), "captured quantized size parameters are supported");
        if (actual) {
            for (std::size_t i = 0; i < actual->size(); ++i) {
                ok &= require(std::fabs(actual->at(i) - item.at("expected")[i].get<float>()) <= 2e-6F,
                              "spatial kernel matches real callback gains");
            }
        }
    }
    ok &= require(!mradm::triple_balance::quantize_size_parameters({0, 0, 0}, std::numeric_limits<float>::quiet_NaN()),
                  "nonfinite extent is rejected");
    const auto input = read_floats(fixtures / "filter-input.f32");
    const auto expected = read_floats(fixtures / "filter-expected.f32");
    ok &= require(input.size() == 2048 && expected.size() == 2048, "complete filter fixture");
    if (input.size() != 2048 || expected.size() != 2048) {
        return 1;
    }
    mradm::triple_balance::SizeDecorrelator filter;
    std::vector<float> filtered;
    for (std::size_t start = 0; start < input.size(); start += 32) {
        std::array<mradm::triple_balance::SizeDecorrelator::FilteredFrame, 32> block{};
        filter.process(std::span<const float, 32>(input.data() + start, 32), block);
        for (const auto& frame : block) {
            filtered.insert(filtered.end(), frame.begin(), frame.end());
        }
    }
    float filter_error = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        filter_error = std::max(filter_error, std::fabs(expected[i] - filtered[(std::size_t{1536} * 4) + i]));
    }
    std::cout << "filter fixture max error: " << filter_error << '\n';
    ok &= require(filter_error <= 2e-6F, "four recursive filter outputs match Renderer capture");

    const auto rapid_input = read_floats(fixtures / "rapid-input.f32");
    const auto rapid_expected = read_floats(fixtures / "rapid-expected.f32");
    const std::vector<mradm::triple_balance::SizeEvent> rapid_events{{0, {0.625F, 0.375F, 0.75F}, 0.25F},
                                                                     {24576, {0.625F, 0.375F, 0.75F}, 0},
                                                                     {28672, {0.625F, 0.375F, 0.75F}, 0.25F}};
    auto rapid = mradm::triple_balance::SizeObjectProcessor::create(rapid_events, "9.1.6", 48000);
    if (!rapid) {
        return 1;
    }
    const auto rapid_output = render(*rapid, rapid_input, 257);
    if (!require(rapid_output.size() == std::size_t{32000} * 16U && rapid_expected.size() == std::size_t{2048} * 16U,
                 "rapid-reset fixtures are complete")) {
        return 1;
    }
    float rapid_error = 0;
    for (std::size_t i = 0; i < rapid_expected.size(); ++i) {
        rapid_error = std::max(rapid_error, std::fabs(rapid_output[(std::size_t{28672} * 16) + i] - rapid_expected[i]));
    }
    ok &= require(rapid_error < 1e-4F,
                  "zero-size block completes its ramp then resets before immediate size restoration");

    std::vector<mradm::triple_balance::SizeEvent> events{{0, {0.5F, 0, 0}, 0.01F},
                                                         {600, {0.25F, 0.6F, 0.5F}, 0.25F},
                                                         {2111, {0.8F, 0.2F, 0.75F}, 1},
                                                         {4096, {0.5F, 0.5F, 1}, 0},
                                                         {6200, {0.5F, 0.5F, 0}, 0.1F}};
    std::vector<float> signal(8457);
    uint32_t noise = 0x73697A65;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        noise = (noise * 1664525U) + 1013904223U;
        signal[i] = i < 7600 ? static_cast<float>(static_cast<int32_t>(noise >> 8) - 8388608) / 67108864.0F : 0;
    }
    for (const auto* layout : {"7.1.4", "9.1.6"}) {
        auto base = mradm::triple_balance::SizeObjectProcessor::create(events, layout, 48000);
        if (!base) {
            return 1;
        }
        const auto wanted = render(*base, signal, 512);
        ok &=
            require(wanted.size() == signal.size() * base->channel_count(), "final partial block retains input length");
        ok &= require(std::ranges::all_of(wanted, [](float x) { return std::isfinite(x); }), "finite sized PCM");
        for (std::size_t frame = 0; frame < signal.size(); ++frame) {
            ok &= require(wanted[(frame * base->channel_count()) + 3] == 0, "Objects do not write LFE");
        }
        for (const std::size_t chunk : {1U, 31U, 32U, 257U, 511U, 512U, 513U, 1024U}) {
            base->reset();
            const auto observed = render(*base, signal, chunk);
            ok &= require(observed == wanted, "reset and arbitrary processing chunks are bit identical");
        }
        auto second = mradm::triple_balance::SizeObjectProcessor::create(events, layout, 48000);
        base->reset();
        std::vector<float> first_output;
        std::vector<float> second_output;
        for (std::size_t at = 0; at < signal.size(); at += 257) {
            const auto block =
                std::span<const float>(signal).subspan(at, std::min<std::size_t>(257, signal.size() - at));
            ok &= require(base->push(block, first_output).has_value(), "first independent object");
            ok &= require(second->push(block, second_output).has_value(), "second independent object");
        }
        ok &= require(base->finish(first_output).has_value() && second->finish(second_output).has_value(),
                      "finish objects");
        ok &= require(first_output == wanted && second_output == wanted, "interleaved instances do not share state");
        ok &= require(!base->push({}, first_output), "finished processor requires reset");
    }
    ok &= require(!mradm::triple_balance::SizeObjectProcessor::create(events, "9.1.6", 44100),
                  "unverified sample rate rejected");
    ok &= require(!mradm::triple_balance::SizeObjectProcessor::create(events, "5.1.4", 48000),
                  "unverified layout rejected");
    events[1].start_sample = 1;
    auto dense = mradm::triple_balance::SizeObjectProcessor::create(events, "7.1.4", 48000);
    auto selected = events;
    selected.erase(selected.begin());
    selected.front().start_sample = 0;
    auto control_reference = mradm::triple_balance::SizeObjectProcessor::create(selected, "7.1.4", 48000);
    ok &= require(dense && control_reference && render(*dense, signal, 257) == render(*control_reference, signal, 512),
                  "dense initial metadata uses the last target without delaying later events");
    events[2].start_sample = 0;
    ok &= require(!mradm::triple_balance::SizeObjectProcessor::create(events, "7.1.4", 48000),
                  "backwards metadata remains rejected");
    return ok ? 0 : 1;
}
