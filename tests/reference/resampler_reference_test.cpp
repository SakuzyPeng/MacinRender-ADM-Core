// Release-only numerical evaluation against the previous production filter.
// Kernel coefficients and phase conventions differ; quality is judged against
// independent tones/timing as well as the C reference, not waveform identity.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <numeric>
#include <samplerate.h>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "resampler.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::vector<float> rust_convert(std::span<const float> input, uint32_t from, uint32_t to, std::size_t channels = 1U) {
    auto r = mradm::dsp::Resampler::create(channels, from, to);
    require(r.has_value(), "create Rust resampler");
    std::vector<float> output(173U * channels);
    std::vector<float> result;
    std::size_t offset = 0U;
    while (offset < input.size()) {
        const auto count = std::min(511U * channels, input.size() - offset);
        auto progress = r->process(input.subspan(offset, count), output);
        require(progress.has_value(), "Rust resampling failed");
        offset += progress->input_frames * channels;
        result.insert(result.end(),
                      output.begin(),
                      output.begin() + static_cast<std::ptrdiff_t>(progress->output_frames * channels));
        require(progress->input_frames != 0U || progress->output_frames != 0U, "Rust resampler made no progress");
    }
    while (true) {
        auto frames = r->finish(output);
        require(frames.has_value(), "Rust finish failed");
        if (*frames == 0U) {
            break;
        }
        result.insert(result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(*frames * channels));
    }
    const auto expected = (static_cast<uint64_t>(input.size() / channels) * to + from - 1U) / from;
    require(result.size() == expected * channels, "Rust rational output length changed");
    return result;
}
std::vector<float> c_convert(std::span<const float> input, uint32_t from, uint32_t to, std::size_t channels = 1U) {
    const auto length = ((static_cast<uint64_t>(input.size() / channels) * to + from - 1U) / from) + 64U;
    std::vector<float> output(static_cast<std::size_t>(length) * channels);
    SRC_DATA data{};
    data.data_in = input.data();
    data.data_out = output.data();
    data.input_frames = static_cast<long>(input.size() / channels);
    data.output_frames = static_cast<long>(length);
    data.src_ratio = static_cast<double>(to) / from;
    data.end_of_input = 1;
    const auto error = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, static_cast<int>(channels));
    if (error != 0) {
        throw std::runtime_error(src_strerror(error));
    }
    require(data.input_frames_used == data.input_frames, "C reference did not consume input");
    output.resize(static_cast<std::size_t>(data.output_frames_gen) * channels);
    return output;
}
std::vector<float> tone(uint32_t rate, double frequency) {
    std::vector<float> input(rate);
    for (std::size_t i = 0U; i < input.size(); ++i) {
        input[i] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate));
    }
    return input;
}
double gain_db(std::span<const float> output, uint32_t rate) {
    const auto margin = static_cast<std::size_t>(rate / 10U);
    require(output.size() > 2U * margin, "short reference audio");
    double energy = 0.0;
    for (const float sample : output.subspan(margin, output.size() - (2U * margin))) {
        require(std::isfinite(sample), "nonfinite resampled PCM");
        energy += static_cast<double>(sample) * sample;
    }
    return 10.0 * std::log10(energy / static_cast<double>(output.size() - (2U * margin)) / 0.125);
}
void evaluate(uint32_t from, uint32_t to, double frequency, bool stopband) {
    const auto input = tone(from, frequency);
    const auto rust = rust_convert(input, from, to);
    const auto native = c_convert(input, from, to);
    const auto rust_db = gain_db(rust, to);
    const auto c_db = gain_db(native, to);
    std::cout << R"({"kind":")" << (stopband ? "stopband" : "passband") << R"(","from":)" << from << R"(,"to":)" << to
              << R"(,"frequency":)" << frequency << R"(,"rust_db":)" << rust_db << R"(,"c_db":)" << c_db << "}\n";
    if (stopband) {
        require(rust_db < -90.0, "Rust alias rejection is below 90 dB");
        require(rust_db < c_db + 3.0, "Rust alias rejection regressed relative to C");
    } else {
        require(std::abs(rust_db) < 0.1 && std::abs(rust_db) < std::abs(c_db) + 0.01,
                "Rust passband response regressed");
    }
}
void short_filters(uint32_t from, uint32_t to) {
    std::vector<float> input(256U, 0.0F);
    input[40U] = 0.5F;
    input[72U] = -0.1F;
    const auto rust = rust_convert(input, from, to);
    const auto native = c_convert(input, from, to);
    const auto peak = [](const std::vector<float>& signal) {
        return std::distance(signal.begin(), std::ranges::max_element(signal, [](float a, float b) {
                                 return std::abs(a) < std::abs(b);
                             }));
    };
    require(!native.empty() && !rust.empty(), "short impulse response was lost");
    require(std::abs(peak(rust) - peak(native)) <= 1, "short filter impulse moved by more than one output sample");
    const double rust_sum = std::accumulate(rust.begin(), rust.end(), 0.0);
    const double c_sum = std::accumulate(native.begin(), native.end(), 0.0);
    std::cout << R"({"kind":"short_hrir","from":)" << from << R"(,"to":)" << to << R"(,"rust_frames":)" << rust.size()
              << R"(,"c_frames":)" << native.size() << R"(,"peak_delta":)" << peak(rust) - peak(native)
              << R"(,"area_ratio":)" << rust_sum / c_sum << "}\n";
    // A short cropped IR does not contain the full sinc ringing, so its area
    // depends on the low-pass kernel and sample-grid phase. Record that delta;
    // the independent long-impulse test checks unity DC gain without truncation.
}
} // namespace
int main() {
    try {
        const std::array<std::pair<uint32_t, uint32_t>, 8> rates{{{48000U, 44100U},
                                                                  {44100U, 48000U},
                                                                  {48000U, 96000U},
                                                                  {96000U, 48000U},
                                                                  {192000U, 44100U},
                                                                  {32000U, 48000U},
                                                                  {192000U, 8000U},
                                                                  {8000U, 192000U}}};
        for (const auto& [from, to] : rates) {
            for (const auto fraction : {0.1, 0.4, 0.45}) {
                evaluate(from, to, std::min(from, to) * fraction, false);
            }
            if (from > to) {
                evaluate(from, to, std::min(to * 0.53, (from + to) / 4.0), true);
            }
            short_filters(from, to);
        }
        for (const auto channels : {2U, 12U, 24U, 64U}) {
            std::vector<float> input(1025U * static_cast<std::size_t>(channels), 0.0F);
            for (std::size_t i = 0U; i < 1025U; ++i) {
                input[i * channels] = std::sin(static_cast<float>(i) * 0.12F) * 0.1F;
            }
            const auto output = rust_convert(input, 48000U, 44100U, channels);
            for (std::size_t i = 0U; i < output.size(); ++i) {
                require(std::isfinite(output[i]) && (i % channels == 0U || output[i] == 0.0F),
                        "multichannel crosstalk");
            }
            std::cout << R"({"kind":"channels","channels":)" << channels << R"(,"frames":)" << output.size() / channels
                      << "}\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
