#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "size_processor.h"

// The compatibility timeline of a late first event drives the size processor as the captured
// Renderer PCM. The copy's report and checks are Rust mradm-render `triple_balance` (PCM
// consistency 4.6, tests/triple_balance.rs); this keeps the captured transition excerpt.
namespace {
bool check(bool condition, const char* message) {
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
    std::vector<float> output(static_cast<std::size_t>(bytes) / sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), bytes);
    return output;
}

// Room-internal coordinates of an ADM Cartesian position.
mradm::triple_balance::SizePosition internal(float x, float y, float z) {
    return {(x + 1) * .5F, (1 - y) * .5F, z};
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path fixtures(argv[1]);
    const auto input = read_floats(fixtures / "input.f32");
    if (!check(input.size() == 52224, "complete late-first-event input fixture")) {
        return 1;
    }
    // An object whose only event is at rtime 48000 (size .25): the copy starts with the default
    // point at the origin, then the event.
    const std::vector<mradm::triple_balance::SizeEvent> events{{0, internal(0, 0, 0), 0},
                                                               {48000, internal(.23F, .51F, .37F), .25F}};
    bool ok = true;
    for (const auto* layout : {"7.1.4", "9.1.6"}) {
        auto processor = mradm::triple_balance::SizeObjectProcessor::create(events, layout, 48000);
        if (!check(processor.has_value(), "compiled semantic timeline initializes DSP")) {
            return 1;
        }
        const auto expected = read_floats(fixtures / (std::string(layout) + ".f32"));
        const auto channels = processor->channel_count();
        if (!check(expected.size() == 5120 * channels, "complete reference transition excerpt")) {
            return 1;
        }
        std::vector<float> baseline;
        for (const std::size_t chunk : {1U, 31U, 32U, 257U, 511U, 512U, 513U, 1024U}) {
            processor->reset();
            std::vector<float> output;
            for (std::size_t at = 0; at < input.size(); at += chunk) {
                ok &= check(processor->push(std::span(input).subspan(at, std::min(chunk, input.size() - at)), output)
                                .has_value(),
                            "chunked semantic PCM renders");
            }
            ok &= check(processor->finish(output).has_value(), "semantic PCM finishes");
            if (baseline.empty()) {
                baseline = output;
            }
            ok &= check(output == baseline, "late initial metadata is independent of caller block size");
        }
        float error = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            error = std::max(error, std::fabs(expected[i] - baseline[(47104 * channels) + i]));
        }
        std::cout << layout << " captured semantic transition max error: " << error << '\n';
        ok &= check(error < 1e-4F, "compiled default/late event matches actual Renderer PCM");
    }
    return ok ? 0 : 1;
}
