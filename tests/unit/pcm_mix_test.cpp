#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "adm/render_ear.h"
#include "adm/render_vbap.h"

#include "../reference/pcm_mix_legacy.h"
#include "pcm_mix.h"
#include "speaker_pcm.h"

namespace {
class Stats {
    std::uint64_t samples{0};
    std::uint64_t differing{0};
    double maximum{0};
    double normalized{0};

  public:
    [[nodiscard]] auto sample_count() const { return samples; }
    [[nodiscard]] auto different_count() const { return differing; }
    [[nodiscard]] auto max_absolute() const { return maximum; }
    [[nodiscard]] auto max_normalized() const { return normalized; }
    void compare(std::span<const float> actual, std::span<const float> expected) {
        if (actual.size() != expected.size()) {
            throw std::runtime_error("PCM length mismatch");
        }
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const double error = std::abs(static_cast<double>(actual[i]) - expected[i]);
            const double scaled = error / (1.0 + std::abs(static_cast<double>(expected[i])));
            if (!std::isfinite(error) || scaled > 2.0e-6) {
                throw std::runtime_error("PCM comparison tolerance exceeded");
            }
            maximum = std::max(maximum, error);
            normalized = std::max(normalized, scaled);
            differing += std::bit_cast<std::uint32_t>(actual[i]) != std::bit_cast<std::uint32_t>(expected[i]) ? 1U : 0U;
            ++samples;
        }
    }
};
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class T> T take(mradm::Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

constexpr std::array<std::uint64_t, 6> k_starts{0U, 3U, 3U, 7U, 14U, 20U};
constexpr std::array<std::uint64_t, 6> k_ends{2U, 8U, 6U, 12U, 30U, 200U};
MradmDspMixBlock descriptor(std::size_t b) {
    return {k_starts.at(b), k_ends.at(b), 17U, (b == 3U ? 1U : 0U) | 2U | (b == 4U ? 4U : 0U)};
}
float coefficient(std::size_t row, std::size_t block, std::size_t output) {
    return static_cast<float>(static_cast<int>(((row + 1U) * (block + 2U) * (output + 1U)) % 19U) - 9) / 16.0F;
}
std::vector<float> signal(std::size_t floats) {
    std::vector<float> result(floats);
    for (std::size_t i = 0; i < floats; ++i) {
        result[i] = static_cast<float>(static_cast<int>((i * 7U) % 31U) - 15) / 16.0F;
    }
    return result;
}

void check_clipped_object_preparation() {
    mradm::NullLogSink logs;
    for (const bool ear : {false, true}) {
        auto renderer = ear ? mradm::create_ear_renderer() : mradm::create_vbap_renderer();
        for (const bool direct_speakers : {false, true}) {
            for (const std::uint64_t end : {0U, 48000U, 144000U}) {
                mradm::RenderPlan plan;
                plan.output_layout = "0+2+0";
                plan.scene.info.sample_rate = 48000U;
                plan.scene.info.num_channels = 1U;
                plan.scene.info.num_frames = 144000U;
                mradm::SceneObject object;
                object.id = "AO_1001";
                object.end_sample = end;
                mradm::SceneTrackRef track;
                track.channel_index = 0U;
                track.track_uid = "ATU_00000001";
                for (const std::uint64_t start : {0U, 96000U}) {
                    if (direct_speakers) {
                        mradm::SceneDirectSpeakersBlock block;
                        block.speaker_labels = {"M+030"};
                        block.start_sample = start;
                        block.end_sample = start == 0U ? 96000U : 144000U;
                        track.ds_blocks.push_back(std::move(block));
                    } else {
                        mradm::SceneObjectBlock block;
                        block.position.azimuth = 30.0F;
                        block.start_sample = start;
                        block.end_sample = start == 0U ? 96000U : 144000U;
                        track.blocks.push_back(block);
                    }
                }
                object.tracks.push_back(std::move(track));
                plan.scene.objects.push_back(std::move(object));
                require(renderer->prepare(plan, logs).has_value(),
                        ear ? "EAR rejects blocks clipped by object duration"
                            : "VBAP rejects blocks clipped by object duration");
            }
        }
    }
}

void check_clipped_timeline() {
    mradm::render_common::ChannelGainInfo channel;
    // Keep clipped blocks as interpolation predecessors when another object uses the same input channel.
    channel.blocks = {{{1.0F}, 0U, 2U, true, true, std::nullopt},
                      {{0.5F}, 4U, 2U, true, true, std::nullopt},
                      {{2.0F}, 6U, 10U, false, true, 4U},
                      {{3.0F}, 12U, 2U, true, true, std::nullopt}};
    auto compiled = take(mradm::render_common::prepare_speaker_mix({channel}, 1U, 1U));
    const std::array<float, 16> expected{
        1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.875F, 1.25F, 1.625F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    for (const std::size_t chunk : {1U, 3U, 16U}) {
        mradm::dsp::PcmMixer mix(compiled.plan, chunk, 0U, false);
        const std::vector<float> input(chunk, 1.0F);
        std::array<float, 16> output{};
        for (std::size_t start = 0; start < output.size(); start += chunk) {
            const auto frames = std::min(chunk, output.size() - start);
            mix.speaker(input, std::span{output}.subspan(start, frames), {}, start, frames);
        }
        require(output == expected, "clipped blocks must stay silent and retain interpolation history");
    }
}

void compare_common(std::uint16_t inputs, std::uint16_t outputs, std::size_t chunk, bool smoothing, Stats& stats) {
    std::vector<pcm_mix_legacy::common::ChannelGainInfo> legacy;
    std::vector<mradm::render_common::ChannelGainInfo> channels;
    for (std::size_t r = 0; r < 4U; ++r) {
        pcm_mix_legacy::common::ChannelGainInfo row;
        row.input_channel = static_cast<std::uint16_t>(r % inputs);
        row.output_gain = r == 1U ? -0.5F : 0.75F;
        mradm::render_common::ChannelGainInfo current;
        current.input_channel = row.input_channel;
        current.output_gain = row.output_gain;
        for (std::size_t b = 0; b < (r == 3U ? 0U : k_starts.size()); ++b) {
            const auto d = descriptor(b);
            std::vector<float> gains(outputs);
            for (std::size_t c = 0; c < outputs; ++c) {
                gains[c] = coefficient(r, b, c);
            }
            const auto length = (d.flags & 4U) != 0U ? std::optional{d.interpolation} : std::nullopt;
            row.blocks.push_back({gains, d.start, d.end, (d.flags & 1U) != 0U, true, length});
            current.blocks.push_back({gains, d.start, d.end, (d.flags & 1U) != 0U, true, length});
        }
        legacy.push_back(std::move(row));
        channels.push_back(std::move(current));
    }
    auto compiled = take(mradm::render_common::prepare_speaker_mix(std::move(channels), inputs, outputs));
    mradm::dsp::PcmMixer mix(compiled.plan, chunk, 9U, smoothing);
    mradm::dsp::PcmMixer second(compiled.plan, chunk, 9U, smoothing);
    compiled.plan = {}; // both instances must retain the Rust table
    const auto input = signal(std::size_t{137U} * inputs);
    std::vector<float> envelope(input.size(), 0.5F);
    std::vector<std::size_t> cursors(legacy.size(), 0U);
    for (std::size_t start = 0; start < 137U; start += chunk) {
        const auto count = std::min(chunk, 137U - start);
        std::vector<float> expected(count * outputs, 0.125F);
        auto actual = expected;
        const auto live = std::span{envelope}.subspan(start * inputs, count * inputs);
        pcm_mix_legacy::common::AccumulateContext ctx{
            input.data() + (start * inputs), &expected, start, inputs, outputs, 9U, smoothing ? 16U : 0U, live};
        pcm_mix_legacy::common::accumulate_gain_matrix(legacy, cursors, ctx, count);
        mix.speaker(std::span{input}.subspan(start * inputs, count * inputs), actual, live, start, count);
        stats.compare(actual, expected);
        second.reset();
        std::ranges::fill(actual, 0.125F);
        second.speaker(std::span{input}.subspan(start * inputs, count * inputs), actual, live, start, count);
        stats.compare(actual, expected);
    }
}

void compare_ear(std::uint16_t inputs, std::uint16_t outputs, std::size_t chunk, bool smoothing, Stats& stats) {
    std::vector<pcm_mix_legacy::ear::ChannelGainInfo> legacy;
    std::vector<MradmDspMixRow> rows;
    std::vector<MradmDspMixBlock> blocks;
    std::vector<double> gains;
    for (std::size_t r = 0; r < 4U; ++r) {
        pcm_mix_legacy::ear::ChannelGainInfo row;
        row.input_channel = static_cast<std::uint16_t>(r % inputs);
        rows.push_back({row.input_channel, blocks.size(), r == 3U ? 0U : k_starts.size(), 1.0F});
        for (std::size_t b = 0; b < rows.back().block_count; ++b) {
            const auto d = descriptor(b);
            std::vector<double> direct(outputs);
            std::vector<double> diffuse(outputs);
            for (std::size_t c = 0; c < outputs; ++c) {
                direct[c] = static_cast<double>(coefficient(r, b, c)) + 1.7e-8;
                diffuse[c] = c % 2U == 0U ? 0.0 : static_cast<double>(coefficient(b, r, c)) - 1.3e-8;
            }
            gains.insert(gains.end(), direct.begin(), direct.end());
            gains.insert(gains.end(), diffuse.begin(), diffuse.end());
            blocks.push_back(d);
            const auto length = (d.flags & 4U) != 0U ? std::optional{d.interpolation} : std::nullopt;
            row.blocks.push_back({direct, diffuse, d.start, d.end, (d.flags & 1U) != 0U, true, length});
        }
        legacy.push_back(std::move(row));
    }
    auto plan = take(mradm::dsp::PcmMixPlan::create(inputs, outputs, rows, blocks, {}, gains, true));
    mradm::dsp::PcmMixer mix(plan, chunk, 9U, smoothing);
    const auto input = signal(std::size_t{137U} * inputs);
    std::vector<std::size_t> cursors(legacy.size(), 0U);
    std::vector<float> mono(chunk);
    for (std::size_t start = 0; start < 137U; start += chunk) {
        const auto count = std::min(chunk, 137U - start);
        std::vector<float> columns(chunk * outputs);
        std::vector<float> diffuse_columns(columns.size());
        std::vector<float> expected(count * outputs);
        std::vector<float> expected_diffuse(expected.size());
        std::vector<float> actual(expected.size(), 42.0F);
        std::vector<float> actual_diffuse(expected.size(), 42.0F);
        pcm_mix_legacy::ear::AccumulateContext ctx{input.data() + (start * inputs),
                                                   columns.data(),
                                                   diffuse_columns.data(),
                                                   start,
                                                   inputs,
                                                   outputs,
                                                   9U,
                                                   smoothing ? 16U : 0U,
                                                   chunk};
        pcm_mix_legacy::ear::accumulate_gain_matrix(legacy, cursors, ctx, count, mono);
        for (std::size_t c = 0; c < outputs; ++c) {
            for (std::size_t f = 0; f < count; ++f) {
                expected[(f * outputs) + c] = columns[(c * chunk) + f];
                expected_diffuse[(f * outputs) + c] = diffuse_columns[(c * chunk) + f];
            }
        }
        mix.ear(std::span{input}.subspan(start * inputs, count * inputs), actual, actual_diffuse, start, count);
        stats.compare(actual, expected);
        stats.compare(actual_diffuse, expected_diffuse);
    }
}

void compare_dynamic(Stats& stats) {
    const std::array<std::size_t, 1> channels{1U};
    auto mixer = mradm::dsp::PcmMixer::dynamic(2U, 2U, channels, 3U, 32U, 9U);
    for (std::size_t phase = 0; phase < 8U; ++phase) {
        const auto start = phase * 32U;
        const std::array<MradmDspMixBlock, 3> blocks{
            {{0U, UINT64_MAX, 0U, 3U}, {start + 3U, UINT64_MAX, 8U, 6U}, {start + 17U, UINT64_MAX, 8U, 6U}}};
        const std::array<float, 6> gains{0.5F, 0.25F, 0.75F, 0.5F, 0.125F, -0.25F};
        mixer.update(0U, blocks, gains, 0.75F);
        pcm_mix_legacy::common::ChannelGainInfo row;
        row.input_channel = 1U;
        row.output_gain = 0.75F;
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto d = blocks.at(b);
            row.blocks.push_back({{gains.at(b * 2U), gains.at((b * 2U) + 1U)},
                                  d.start,
                                  d.end,
                                  (d.flags & 1U) != 0U,
                                  true,
                                  (d.flags & 4U) != 0U ? std::optional{d.interpolation} : std::nullopt});
        }
        const auto input = signal(64U);
        std::vector<float> expected(64U, 0.125F);
        auto actual = expected;
        const std::vector<float> live(64U, 0.5F);
        std::size_t index = 0;
        pcm_mix_legacy::common::AccumulateContext ctx{input.data(), &expected, start, 2U, 2U, 9U, 0U, live};
        pcm_mix_legacy::common::accumulate_speaker_channel(row, index, ctx, 32U);
        mixer.speaker(input, actual, live, start, 32U, 0U);
        stats.compare(actual, expected);
        std::ranges::fill(actual, 0.125F);
        mixer.speaker(input, actual, live, start, 32U, 0U);
        stats.compare(actual, expected);
        row.output_gain = 1.0F;
        index = 0U;
        std::ranges::fill(expected, 0.125F);
        std::ranges::fill(actual, 0.125F);
        pcm_mix_legacy::common::accumulate_speaker_channel(row, index, ctx, 32U);
        mixer.speaker(input, actual, live, start, 32U, 0U, 1.0F);
        stats.compare(actual, expected);
    }
}

void compare_matrix(Stats& stats) {
    for (const std::uint32_t inputs : {1U, 2U, 12U, 64U}) {
        for (const std::uint32_t outputs : {1U, 2U, 6U, 24U}) {
            const auto gains = signal(static_cast<std::size_t>(inputs) * outputs);
            const auto input = signal(std::size_t{137U} * inputs);
            std::vector<float> expected(std::size_t{137U} * outputs);
            std::vector<float> actual(expected.size());
            mradm::dsp::PcmMatrix matrix(inputs, outputs, gains);
            pcm_mix_legacy::matrix(input, expected, gains, inputs, outputs, 137U);
            require(matrix.process(input, actual, 137U).has_value(), "fixed matrix failed");
            stats.compare(actual, expected);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        check_clipped_object_preparation();
        check_clipped_timeline();
        Stats speaker;
        Stats ear;
        Stats dynamic;
        Stats matrix;
        for (const auto inputs : std::array<std::uint16_t, 4>{1U, 2U, 12U, 64U}) {
            for (const auto outputs : std::array<std::uint16_t, 5>{1U, 2U, 6U, 12U, 24U}) {
                for (const auto frames : {1U, 7U, 37U, 128U}) {
                    for (const bool smoothing : {false, true}) {
                        compare_common(inputs, outputs, frames, smoothing, speaker);
                        compare_ear(inputs, outputs, frames, smoothing, ear);
                    }
                }
            }
        }
        compare_dynamic(dynamic);
        compare_matrix(matrix);
        const auto report = [&](std::ostream& out) {
            out << std::setprecision(17)
                << R"json({"baseline_commit":"4286f3e","tolerance":"2e-6 + 2e-6 * abs(reference)","cases":[)json"
                << '\n';
            const std::array names{"speaker", "ear", "dynamic", "matrix"};
            const std::array cases{speaker, ear, dynamic, matrix};
            for (std::size_t i = 0; i < cases.size(); ++i) {
                const auto& s = cases.at(i);
                out << R"json(    {"kind": ")json" << names.at(i) << R"json(", "samples": )json" << s.sample_count()
                    << R"json(, "differing_samples": )json" << s.different_count()
                    << R"json(, "max_absolute_error": )json" << s.max_absolute()
                    << R"json(, "max_normalized_error": )json" << s.max_normalized() << '}'
                    << (i + 1U == cases.size() ? "\n" : ",\n");
            }
            out << "  ]\n}\n";
        };
        report(std::cout);
        if (argc == 2) {
            std::ofstream file(argv[1]);
            report(file);
            require(file.good(), "cannot write report");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
