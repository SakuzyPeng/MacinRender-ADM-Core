#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "../reference/triple_balance/motion.h"
#include "../reference/triple_balance/render_state.h"
#include "../reference/triple_balance/size_processor.h"
#include "layout_222.h"
#include "panner.h"
#include "render_state.h"
#include "size_processor.h"
namespace {
namespace old = mradm::triple_balance_legacy;
namespace now = mradm::triple_balance;
using namespace mradm;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}
void done(Result<void> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}
class Stats {
  private:
    uint64_t samples{}, different{};
    double maximum{}, normalized{};

  public:
    void compare(std::span<const float> a, std::span<const float> b) {
        require(a.size() == b.size(), "comparison length");
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double error = std::fabs(double(a[i]) - b[i]);
            const double limit = 2e-6 + (2e-6 * std::fabs(double(a[i])));
            ++samples;
            different += static_cast<uint64_t>(std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(b[i]));
            maximum = std::max(maximum, error);
            normalized = std::max(normalized, error / limit);
            if (!std::isfinite(error) || error > limit) {
                std::cerr << "difference at " << i << ": " << a[i] << " / " << b[i] << " error=" << error << '\n';
                throw std::runtime_error("legacy numerical tolerance");
            }
        }
    }
    [[nodiscard]] nlohmann::json json() const {
        return {{"samples", samples},
                {"differing_samples", different},
                {"max_absolute_error", maximum},
                {"max_normalized_error", normalized}};
    }
};
SceneBlockPosition position(float x, float y, float z) {
    SceneBlockPosition p;
    p.cartesian = true;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}
std::vector<float> signal(std::size_t frames) {
    std::vector<float> result(frames);
    uint32_t state = 0x91c0ffee;
    for (std::size_t i = 0; i < frames; ++i) {
        state = (state * 1664525U) + 1013904223U;
        result[i] = i % 17000 < 15500 ? float(int32_t(state >> 8U) - 8388608) / 33554432.0F : 0;
    }
    return result;
}
void spatial(Stats& stats) {
    for (const auto* layout : {"7.1.4", "9.1.6", "22.2"}) {
        for (float x : {-1.F, -.5F, 0.F, .5F, 1.F}) {
            for (float y : {-1.F, -.5F, 0.F, .5F, 1.F}) {
                for (float z : {0.F, .1F, .5F, 1.F}) {
                    for (float xx : {std::nextafter(x, -1.F), x, std::nextafter(x, 1.F)}) {
                        auto p = position(xx, y, z);
                        stats.compare(take(old::point_gains(p, .73F, layout)), take(now::point_gains(p, .73F, layout)));
                    }
                }
            }
        }
    }
    for (int code = 0; code < 32768; code += 127) {
        const float center = (float(code) + .5F) / 32768;
        for (float x : {std::nextafter(center, 0.F), center, std::nextafter(center, 1.F)}) {
            auto a = take(old::quantize_size_parameters({x, .5F, 1}, x));
            auto b = take(now::quantize_size_parameters({x, .5F, 1}, x));
            require(a.xyz == b.xyz && a.size == b.size, "Q15 boundary exact");
            auto aa = take(old::raw_size_gains(a));
            auto bb = take(now::raw_size_gains(b));
            stats.compare(aa, bb);
            const auto am = old::mix_size_gains(aa, x);
            const auto bm = now::mix_size_gains(bb, x);
            stats.compare(am.spread, bm.spread);
            stats.compare(std::span{&am.direct, 1}, std::span{&bm.direct, 1});
        }
    }
    for (float z : {-1.F, -.5F, 0.F, .5F, 1.F}) {
        for (float size : {0.F, .00001F, .01F, .19999F, .2F, .75F, 1.F}) {
            auto p = position(.37F, -.19F, z);
            auto a = take(old::room_222_gains(p, size));
            auto b = take(now::room_222_gains(p, size));
            stats.compare(a, b);
            stats.compare(old::room_222_mix(a, size).spread, now::room_222_mix(b, size).spread);
        }
    }
}
template <class P> std::vector<float> render(P& p, std::span<const float> source, std::size_t chunk) {
    std::vector<float> out;
    for (std::size_t i = 0; i < source.size(); i += chunk) {
        done(p.push(source.subspan(i, std::min(chunk, source.size() - i)), out));
    }
    done(p.finish(out));
    return out;
}
void objects(Stats& stats) {
    const auto pcm = signal(32037);
    for (const auto* layout : {"7.1.4", "9.1.6", "22.2"}) {
        const std::vector<old::SizeEvent> events{{0, {.625F, .375F, .75F}, .25F},
                                                 {600, {.25F, .6F, .5F}, .01F},
                                                 {2111, {.8F, .2F, .75F}, 1},
                                                 {24576, {.625F, .375F, .75F}, 0},
                                                 {28672, {.625F, .375F, .75F}, .25F}};
        std::vector<now::SizeEvent> current;
        current.reserve(events.size());
        std::ranges::transform(events, std::back_inserter(current), [](auto e) {
            return now::SizeEvent{e.start_sample, {e.position.x, e.position.y, e.position.z}, e.size};
        });
        auto reference = take(old::SizeObjectProcessor::create(events, layout, 48000));
        const auto expected = render(reference, pcm, 1024);
        std::vector<float> canonical;
        for (std::size_t chunk : {1U, 31U, 32U, 33U, 257U, 511U, 512U, 513U, 1023U, 1024U}) {
            auto p = take(now::SizeObjectProcessor::create(current, layout, 48000));
            auto actual = render(p, pcm, chunk);
            stats.compare(expected, actual);
            if (canonical.empty()) {
                canonical = actual;
            } else {
                require(canonical == actual, "object chunk bit identity");
            }
            p.reset();
            require(render(p, pcm, chunk) == actual, "reset bit identity");
        }
        auto p = take(now::SizeObjectProcessor::create(current, layout, 48000));
        std::vector<float> prefix;
        done(p.push(std::span{pcm}.first(257), prefix));
        auto state = p.snapshot();
        std::vector<float> a;
        std::vector<float> b;
        done(p.push(std::span{pcm}.subspan(257, 2048), a));
        p.restore(state);
        done(p.push(std::span{pcm}.subspan(257, 2048), b));
        require(a == b, "pending snapshot exact");
    }
    old::SizeDecorrelator a;
    now::SizeDecorrelator b;
    for (std::size_t i = 0; i + 32 <= pcm.size(); i += 32) {
        std::array<old::SizeDecorrelator::FilteredFrame, 32> x{};
        std::array<now::SizeDecorrelator::FilteredFrame, 32> y{};
        const auto in = std::span<const float, 32>{pcm.data() + i, 32};
        a.process(in, x);
        b.process(in, y);
        stats.compare({x.front().data(), 128}, {y.front().data(), 128});
    }
}
void state_decisions() {
    for (const auto* layout : {"7.1.4", "9.1.6", "22.2"}) {
        const std::vector<old::SizeEvent> events{{0, {.125F, .625F, .25F}, .25F},
                                                 {600, {.5F, .5F, .5F}, .2F},
                                                 {4096, {.5F, .5F, .5F}, 0},
                                                 {28672, {.5F, .5F, .5F}, .25F},
                                                 {32768, {.75F, .25F, .75F}, 1}};
        std::vector<now::SizeEvent> converted;
        converted.reserve(events.size());
        std::ranges::transform(events, std::back_inserter(converted), [](auto e) {
            return now::SizeEvent{e.start_sample, {e.position.x, e.position.y, e.position.z}, e.size};
        });
        auto a = take(old::SizeObjectProcessor::create(events, layout, 48000));
        auto b = take(now::SizeObjectProcessor::create(converted, layout, 48000));
        const std::array<float, 512> silence{};
        std::vector<float> x;
        std::vector<float> y;
        for (int block = 0; block < 160; ++block) {
            x.clear();
            y.clear();
            done(a.push(silence, x));
            done(b.push(silence, y));
            const auto old_state = a.snapshot();
            const auto state = b.state_info();
            require(state.control == old_state.control_start_ && state.next_event == old_state.next_event_ &&
                        state.pending == old_state.pending_frames_,
                    "control/event state exact");
            const uint32_t flags =
                uint32_t(old_state.first_) | (uint32_t(old_state.finished_) << 1U) |
                (uint32_t(old_state.previous_filter_active_) << 2U) | (uint32_t(old_state.older_filter_active_) << 3U) |
                (uint32_t((std::string_view(layout) == "22.2" ? old_state.previous_extended_mix_.direct
                                                              : old_state.previous_mix_.direct) == 0)
                 << 4U);
            require(state.flags == flags, "filter/direct lifecycle branch exact");
            if (std::string_view(layout) != "22.2") {
                const auto q = take(old::quantize_size_parameters(old_state.position_, old_state.size_));
                require(std::equal(q.xyz.begin(), q.xyz.end(), std::begin(state.quantized)) &&
                            q.size == state.quantized[3],
                        "smoothed quantization code exact");
            }
        }
    }
}
// NOLINTNEXTLINE(readability-function-size): paired legacy/new motion and transition fixture.
void curves_and_mix(Stats& stats) {
    const auto mono = signal(9237);
    std::vector<float> input(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        input[i * 2] = mono[i];
        input[(i * 2) + 1] = mono[mono.size() - 1 - i];
    }
    for (const auto* layout : {"7.1.4", "9.1.6", "22.2"}) {
        RenderPlan plan;
        plan.output_layout = layout;
        plan.scene.info.sample_rate = 48000;
        plan.scene.info.num_channels = 2;
        plan.scene.info.num_frames = mono.size();
        const std::size_t channels = dsp::tb_channels(*dsp::tb_layout(layout));
        SceneTrackRef track;
        for (uint64_t start : {0U, 600U, 2111U, 4096U, 6200U}) {
            SceneObjectBlock e;
            e.start_sample = start;
            e.position = position(start == 0 ? .25F : -.6F, .25F, .5F);
            e.width = .25F;
            track.blocks.push_back(e);
        }
        SceneObject object;
        object.end_sample = mono.size();
        auto motion = take(old::triple_balance_motion_blocks(track, object, layout, mono.size()));
        render_common::ChannelGainInfo channel;
        channel.input_channel = 0;
        channel.output_gain = .73F;
        channel.blocks = std::move(motion);
        std::vector<render_common::ChannelGainInfo> list;
        list.push_back(std::move(channel));
        auto old_plan = take(render_common::prepare_speaker_mix(std::move(list), 2, channels));
        std::vector<MradmTbEvent> events;
        events.reserve(track.blocks.size());
        std::ranges::transform(track.blocks, std::back_inserter(events), [](const auto& e) {
            return MradmTbEvent{e.start_sample, {e.position.x, e.position.y, e.position.z}, e.width};
        });
        const MradmTbRow row{0, 0, events.size(), 0, 0, 1, .73F};
        auto numeric =
            take(dsp::TbPlan::create(2, *dsp::tb_layout(layout), 48000, mono.size(), std::span{&row, 1}, events, {}));
        auto mixplan = take(numeric.mix_plan());
        dsp::PcmMixer a(old_plan.plan, 1024, 960, false);
        dsp::PcmMixer b(mixplan, 1024, 960, false);
        for (std::size_t start = 0; start < mono.size(); start += 1024) {
            const auto frames = std::min<std::size_t>(1024, mono.size() - start);
            std::vector<float> x(frames * channels, .125F);
            std::vector<float> y = x;
            std::vector<float> live(frames * 2, .81F);
            auto src = std::span{input}.subspan(start * 2, frames * 2);
            a.speaker(src, x, live, start, frames);
            b.speaker(src, y, live, start, frames);
            stats.compare(x, y);
        }
        old::Prepared legacy;
        legacy.output_channels = uint16_t(channels);
        legacy.sample_rate = 48000;
        old::SizeTrack sized;
        sized.input_channel = 0;
        sized.output_gain = .73F;
        sized.initial_position = track.blocks[0].position;
        for (auto e : track.blocks) {
            sized.events.push_back(
                {e.start_sample, {(e.position.x + 1) * .5F, (1 - e.position.y) * .5F, e.position.z}, e.width});
        }
        legacy.size_tracks.push_back(sized);
        auto old_mix = take(old::SizeMixer::create(legacy, plan, true));
        now::Prepared prepared;
        prepared.output_channels = uint16_t(channels);
        prepared.sample_rate = 48000;
        auto size_row = row;
        size_row.kind = 2;
        prepared.numeric = take(
            dsp::TbPlan::create(2, *dsp::tb_layout(layout), 48000, mono.size(), std::span{&size_row, 1}, events, {}));
        auto new_mix = take(now::SizeMixer::create(prepared, plan, true));
        for (std::size_t start = 0; start < mono.size(); start += 1024) {
            constexpr std::array<float, 4> steps{1, 0, .5F, 4};
            const float scale = steps.at(std::min<std::size_t>(start / 2048, 3));
            const std::array scales{scale};
            old_mix.set_scales(scales, start == 0);
            new_mix.set_scales(scales, start == 0);
            const auto frames = std::min<std::size_t>(1024, mono.size() - start);
            done(old_mix.prepare_points(start, frames));
            done(new_mix.prepare_points(start, frames));
            std::vector<float> x(frames * channels, .125F);
            std::vector<float> y = x;
            std::vector<float> live(frames * 2, .81F);
            auto src = std::span{input}.subspan(start * 2, frames * 2);
            render_common::AccumulateContext cx{src.data(), &x, start, 2, uint16_t(channels), 960, 0, live};
            auto cy = cx;
            cy.output = &y;
            old_mix.accumulate_point(0, cx, frames, true);
            new_mix.accumulate_point(0, cy, frames, true);
            done(old_mix.process(src, x, start, start + frames == mono.size(), live));
            done(new_mix.process(src, y, start, start + frames == mono.size(), live));
            stats.compare(x, y);
        }
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        Stats spatial_stats;
        Stats object_stats;
        Stats mix_stats;
        spatial(spatial_stats);
        state_decisions();
        objects(object_stats);
        curves_and_mix(mix_stats);
        nlohmann::json result{{"baseline", "19728179608c4cfe550dc11ff2b3b4425db37963"},
                              {"spatial", spatial_stats.json()},
                              {"objects_and_filter", object_stats.json()},
                              {"motion_and_mix", mix_stats.json()},
                              {"control_blocks_with_exact_decisions", 480},
                              {"passed", true}};
        std::cout << result.dump(2) << '\n';
        if (argc == 2) {
            std::ofstream(argv[1]) << result.dump(2) << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
