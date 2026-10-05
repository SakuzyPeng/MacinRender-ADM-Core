#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../reference/hoa/legacy.h"
#include "hoa.h"
namespace {
using namespace mradm;
namespace old = hoa_legacy;
void require(bool b, const char* m) {
    if (!b) {
        throw std::runtime_error(m);
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
  public:
    void compare(std::span<const float> expected, std::span<const float> actual) {
        require(expected.size() == actual.size(), "comparison length");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            ++count_;
            if (std::bit_cast<uint32_t>(expected[i]) != std::bit_cast<uint32_t>(actual[i])) {
                ++different_;
            }
            if (!std::isfinite(expected[i]) || !std::isfinite(actual[i])) {
                require((std::isnan(expected[i]) && std::isnan(actual[i])) || expected[i] == actual[i],
                        "nonfinite propagation");
                continue;
            }
            const double error = std::fabs(double(expected[i]) - actual[i]);
            const double limit = 2e-6 + (2e-6 * std::fabs(double(expected[i])));
            maximum_ = std::max(maximum_, error);
            ratio_ = std::max(ratio_, error / limit);
            if (error > limit) {
                std::cerr << "sample=" << i << " expected=" << expected[i] << " actual=" << actual[i]
                          << " error=" << error << '\n';
                throw std::runtime_error("numerical tolerance");
            }
        }
    }
    [[nodiscard]] nlohmann::json json() const {
        return {{"samples", count_},
                {"differing_samples", different_},
                {"max_absolute_error", maximum_},
                {"max_tolerance_ratio", ratio_}};
    }

  private:
    uint64_t count_{}, different_{};
    double maximum_{}, ratio_{};
};
struct Order {
    uint64_t start;
    std::size_t index;
};
struct Case {
    AdmScene scene;
    std::vector<MradmHoaSource> sources;
    std::vector<MradmHoaBlock> blocks;
    std::map<uint16_t, std::vector<Order>> channels;
};
MradmHoaSource convert(const SceneObjectBlock& b) {
    const auto& p = b.position;
    return {{p.cartesian ? p.x : p.azimuth, p.cartesian ? p.y : p.elevation, p.cartesian ? p.z : p.distance},
            p.cartesian ? 1U : 0U,
            b.width,
            b.height,
            b.depth,
            b.gain,
            b.diffuse};
}
void add_object(Case& c, uint16_t channel, const std::vector<SceneObjectBlock>& blocks, float gain = 1) {
    SceneObject obj;
    obj.id = "object-" + std::to_string(c.scene.objects.size());
    obj.gain = gain;
    SceneTrackRef track;
    track.channel_index = channel;
    track.blocks = blocks;
    for (const auto& b : blocks) {
        const auto expanded = expand_object_divergence(b);
        const auto start = c.sources.size();
        for (const auto& e : expanded) {
            c.sources.push_back(convert(e));
        }
        c.channels[channel].push_back({b.start_sample, c.blocks.size()});
        c.blocks.push_back({b.start_sample,
                            b.end_sample,
                            b.interp_length_samples.value_or(0),
                            start,
                            expanded.size(),
                            gain,
                            0U,
                            (b.jump_position ? 1U : 0U) | (b.interp_length_samples ? 2U : 0U)});
    }
    obj.tracks.push_back(std::move(track));
    c.scene.objects.push_back(std::move(obj));
}
void add_speakers(Case& c, uint16_t channel, const std::vector<SceneDirectSpeakersBlock>& blocks, float gain = 1) {
    SceneObject obj;
    obj.id = "speaker-" + std::to_string(c.scene.objects.size());
    obj.gain = gain;
    SceneTrackRef track;
    track.channel_index = channel;
    track.ds_blocks = blocks;
    for (const auto& b : blocks) {
        const bool lfe = render_common::direct_speakers_block_is_lfe(b);
        const auto offset = c.sources.size();
        c.sources.push_back({{b.azimuth, b.elevation, 1}, 0, 0, 0, 0, b.gain, 0});
        c.channels[channel].push_back({b.start_sample, c.blocks.size()});
        c.blocks.push_back({b.start_sample, b.end_sample, 0, offset, 1, gain, lfe ? 2U : 1U, 1U});
    }
    obj.tracks.push_back(std::move(track));
    c.scene.objects.push_back(std::move(obj));
}
dsp::HoaPlan compile(Case& c, MradmHoaTrace* trace = nullptr) {
    std::vector<MradmHoaRow> rows;
    std::vector<std::size_t> order;
    for (auto& [channel, list] : c.channels) {
        std::ranges::sort(list, {}, &Order::start);
        rows.push_back({channel, order.size(), list.size()});
        std::ranges::transform(list, std::back_inserter(order), [](auto block) { return block.index; });
    }
    return take(dsp::HoaPlan::create(c.scene.info.num_channels, rows, c.blocks, order, c.sources, trace));
}
SceneObjectBlock block(uint64_t start, float az, float el, float diffuse = 0) {
    SceneObjectBlock b;
    b.start_sample = start;
    b.position.azimuth = az;
    b.position.elevation = el;
    b.diffuse = diffuse;
    return b;
}
SceneDirectSpeakersBlock speaker(uint64_t start, bool lfe) {
    SceneDirectSpeakersBlock b;
    b.start_sample = start;
    b.has_position = true;
    b.azimuth = 30;
    b.elevation = 0;
    b.speaker_labels = {lfe ? "RC_LFE" : "M+030"};
    return b;
}
std::vector<float> signal(std::size_t frames, std::size_t channels) {
    std::vector<float> data(frames * channels);
    uint32_t state = 0x484f4133;
    for (auto& v : data) {
        state = (state * 1664525U) + 1013904223U;
        v = float(int32_t(state >> 8U) - 8388608) / 67108864.0F;
    }
    return data;
}
void compare_coefficients(Case& c, Stats& stats) {
    NullLogSink logs;
    const auto ref = old::build_gain_matrix(c.scene, logs);
    auto plan = compile(c);
    require(ref.size() == c.channels.size(), "row count");
    for (std::size_t r = 0; r < ref.size(); ++r) {
        for (std::size_t b = 0; b < ref[r].blocks.size(); ++b) {
            std::array<float, 64> values{};
            done(dsp::hoa_status(mradm_dsp_hoa_coefficients, plan.get(), r, b, values.data(), values.size()));
            const auto all = std::span{values};
            stats.compare(ref[r].blocks[b].gains, all.first<16>());
            for (std::size_t slot = 0; slot < 3; ++slot) {
                stats.compare(ref[r].blocks[b].diffuse_gains.at(slot), all.subspan((slot + 1) * 16, 16));
            }
        }
    }
}
void spatial(Stats& stats) {
    for (bool cartesian : {false, true}) {
        for (float az : {-179.F, -90.F, -.01F, 0.F, 30.F, 90.F, 179.F}) {
            for (float elevation : {-90.F, -.01F, 0.F, 45.F, 90.F}) {
                for (float extent : {0.F, .000001F, .00001F, .1F, 1.F, 3.F}) {
                    Case c;
                    c.scene.info.num_channels = 1;
                    auto b = block(0, az, elevation, .35F);
                    b.width = extent;
                    b.height = extent * .3F;
                    b.depth = extent * .2F;
                    b.position.cartesian = cartesian;
                    b.position.x = az / 180;
                    b.position.y = elevation / 90;
                    b.position.z = .375F;
                    b.divergence = extent > 0.1F ? .6F : 0.F;
                    b.divergence_azimuth_range = 67;
                    b.divergence_position_range = .2F;
                    add_object(c, 0, {b}, .73F);
                    compare_coefficients(c, stats);
                }
            }
        }
    }
    for (float coordinate : {0.F, 1e-8F, 1e-6F, 1e-4F, .25F, 1.F}) {
        Case c;
        c.scene.info.num_channels = 1;
        auto b = block(0, 0, 0);
        b.position.cartesian = true;
        b.position.x = coordinate;
        b.position.y = coordinate;
        b.position.z = coordinate;
        add_object(c, 0, {b});
        compare_coefficients(c, stats);
    }
    // Trace values follow original scene traversal rather than the sorted PCM channel table.
    Case c;
    c.scene.info.num_channels = 2;
    auto first = block(0, 30, 15);
    add_object(c, 1, {first});
    add_object(c, 0, {block(0, -45, 0)});
    MradmHoaTrace trace{};
    auto plan = compile(c, &trace);
    (void) plan;
    require(trace.flags == 3, "trace stage flags");
    const auto dir = old::direction_from_polar(30, 15);
    const auto normal = old::normalize(dir);
    const std::array wanted_dir{dir.x, dir.y, dir.z};
    const std::array wanted_normal{normal.x, normal.y, normal.z};
    stats.compare(wanted_dir, trace.direction);
    stats.compare(wanted_normal, trace.normalized);
    stats.compare(old::sh_sn3d_3(normal.x, normal.y, normal.z), trace.coefficients);
}
Case timeline_case() {
    Case c;
    c.scene.info.num_channels = 4;
    auto a = block(37, 0, 0, .3F);
    a.end_sample = 200;
    auto b = block(500, 45, 25, 1);
    b.end_sample = 470;
    auto d = block(500, -30, -15, .7F);
    d.interp_length_samples = 9000;
    d.width = .5F;
    auto e = block(1300, 100, 60, 0);
    e.jump_position = true;
    e.end_sample = 2048;
    auto f = block(4096, -65, 30, .4F);
    f.height = .4F;
    f.divergence = .8F;
    f.divergence_azimuth_range = 90;
    add_object(c, 0, {a, b, d, e, f}, .75F);
    auto l0 = speaker(0, true);
    l0.end_sample = 500;
    auto l1 = speaker(768, true);
    l1.gain = .5F;
    l1.end_sample = 3000;
    auto l2 = speaker(4096, false);
    add_speakers(c, 1, {l0, l1, l2}, .5F);
    auto mixed = block(512, -90, 0);
    mixed.end_sample = 767;
    add_object(c, 1, {mixed}, .25F);
    auto divergent = block(0, 30, 10, 1);
    divergent.divergence = .8F;
    divergent.divergence_azimuth_range = 45;
    add_object(c, 2, {divergent, block(2200, -30, 40, .2F)}, .5F);
    // Empty bound row is retained; zero coefficients still have the original IEEE PCM behavior.
    SceneObject empty;
    SceneTrackRef t;
    t.channel_index = 3;
    empty.tracks.push_back(t);
    c.scene.objects.push_back(empty);
    c.channels[3] = {};
    return c;
}
void pcm_and_meter(Stats& pcm, Stats& decoded, Stats& lfe) {
    auto c = timeline_case();
    NullLogSink logs;
    const auto ref = old::build_gain_matrix(c.scene, logs);
    auto plan = compile(c);
    std::array<float, 176> matrix{};
    done(dsp::hoa_status(mradm_dsp_hoa_matrix, matrix.data(), matrix.size()));
    const auto input = signal(8197, 4);
    for (bool smooth : {false, true}) {
        for (std::size_t capacity : {1024U, 1537U, 2048U}) {
            auto encoder = take(dsp::HoaEncoder::create(plan, capacity, 240, smooth));
            auto meter = take(dsp::HoaMeterPreprocessor::create(plan, capacity, 240));
            std::vector<std::array<old::DiffuseState, 3>> state(ref.size());
            std::vector<float> scratch(capacity * 16);
            std::vector<float> xd(capacity * 12);
            std::vector<float> yd(xd.size());
            std::vector<float> lfe_reference(capacity);
            std::vector<float> lfe_actual(capacity);
            for (std::size_t start = 0; start < 8197;) {
                const auto frames = std::min(capacity, std::size_t{8197} - start);
                std::vector<float> x(frames * 16, .125F);
                std::vector<float> y = x;
                const auto source = std::span{input}.subspan(start * 4, frames * 4);
                old::encode_hoa_block(
                    ref, state, source.data(), x.data(), start, frames, 4, 240, smooth ? uint32_t(capacity) : 0U);
                done(encoder.process(source, y, start, frames));
                pcm.compare(x, y);
                old::measure(ref, source.data(), x.data(), start, frames, 4, 240, matrix, scratch, xd, lfe_reference);
                done(meter.process(source, y, yd, lfe_actual, start, frames));
                decoded.compare(std::span{xd}.first(frames * 12), std::span{yd}.first(frames * 12));
                lfe.compare(std::span{lfe_reference}.first(frames), std::span{lfe_actual}.first(frames));
                start += frames;
            }
            // Cold non-aligned seek, retaining the original modulo write position.
            constexpr uint64_t start = 1137;
            encoder.reset(start);
            for (auto& slots : state) {
                for (auto& slot : slots) {
                    slot = old::DiffuseState{};
                    slot.write_pos = start % 1024;
                }
            }
            const auto frames = std::min(capacity, std::size_t{513});
            std::vector<float> x(frames * 16, .125F);
            std::vector<float> y = x;
            auto source = std::span{input}.subspan(start * 4, frames * 4);
            old::encode_hoa_block(
                ref, state, source.data(), x.data(), start, frames, 4, 240, smooth ? uint32_t(capacity) : 0U);
            done(encoder.process(source, y, start, frames));
            pcm.compare(x, y);
        }
    }
}
struct Metrics {
    double loudness;
    double peak;
    bool has_loudness;
};
Metrics metrics(const std::vector<float>& decoded, const std::vector<float>& lfe) {
    using Ch = dsp::MeterChannel;
    constexpr std::array<Ch, 12> map{Ch::left,
                                     Ch::right,
                                     Ch::center,
                                     Ch::unused,
                                     Ch::side_left,
                                     Ch::side_right,
                                     Ch::rear_left,
                                     Ch::rear_right,
                                     Ch::top_front_left,
                                     Ch::top_front_right,
                                     Ch::top_rear_left,
                                     Ch::top_rear_right};
    auto meter = take(dsp::Meter::create(12, 48000, dsp::MeterMode::integrated_true_peak, map));
    auto low = take(dsp::Meter::create(1, 48000, dsp::MeterMode::true_peak));
    done(meter.add_frames(decoded.data(), decoded.size() / 12));
    done(low.add_frames(lfe.data(), lfe.size()));
    const auto loudness = meter.integrated();
    const auto peak = std::max(take(meter.max_true_peak()), take(low.max_true_peak()));
    return {loudness.value_or(0), peak > 0 ? 20 * std::log10(peak) : 0, loudness && std::isfinite(*loudness)};
}
nlohmann::json metric_comparison() {
    auto c = timeline_case();
    NullLogSink logs;
    const auto ref = old::build_gain_matrix(c.scene, logs);
    auto plan = compile(c);
    auto encoder = take(dsp::HoaEncoder::create(plan, 1024, 240, false));
    auto meter = take(dsp::HoaMeterPreprocessor::create(plan, 1024, 240));
    std::array<float, 176> matrix{};
    done(dsp::hoa_status(mradm_dsp_hoa_matrix, matrix.data(), matrix.size()));
    const auto input = signal(48037, 4);
    std::vector<std::array<old::DiffuseState, 3>> state(ref.size());
    std::vector<float> xd(std::size_t{48037} * 12);
    std::vector<float> yd(xd.size());
    std::vector<float> lfe_reference(48037);
    std::vector<float> lfe_actual(48037);
    std::vector<float> scratch(std::size_t{1024} * 16);
    std::vector<float> chunk(std::size_t{1024} * 12);
    std::vector<float> lfec(1024);
    for (std::size_t start = 0; start < 48037; start += 1024) {
        const auto frames = std::min(std::size_t{1024}, std::size_t{48037} - start);
        std::vector<float> x(frames * 16);
        std::vector<float> y(x.size());
        const auto src = std::span{input}.subspan(start * 4, frames * 4);
        old::encode_hoa_block(ref, state, src.data(), x.data(), start, frames, 4, 240, 0);
        done(encoder.process(src, y, start, frames));
        old::measure(ref, src.data(), x.data(), start, frames, 4, 240, matrix, scratch, chunk, lfec);
        std::copy_n(chunk.begin(), frames * 12, xd.begin() + std::ptrdiff_t(start * 12));
        std::copy_n(lfec.begin(), frames, lfe_reference.begin() + std::ptrdiff_t(start));
        done(meter.process(src,
                           y,
                           std::span{yd}.subspan(start * 12, frames * 12),
                           std::span{lfe_actual}.subspan(start, frames),
                           start,
                           frames));
    }
    const auto a = metrics(xd, lfe_reference);
    const auto b = metrics(yd, lfe_actual);
    require(a.has_loudness == b.has_loudness, "metric presence");
    const double lu = std::fabs(a.loudness - b.loudness);
    const double peak = std::fabs(a.peak - b.peak);
    require(lu <= 1e-4 && peak <= 1e-4, "metric tolerance");
    return {{"lufs_difference", lu}, {"dbtp_difference", peak}, {"has_loudness", a.has_loudness}};
}
} // namespace
int main(int argc, char** argv) {
    try {
        Stats coefficients;
        Stats pcm;
        Stats decoded;
        Stats lfe;
        spatial(coefficients);
        pcm_and_meter(pcm, decoded, lfe);
        const auto result = nlohmann::json{{"baseline", "ba96bac"},
                                           {"coefficients", coefficients.json()},
                                           {"encoded_pcm", pcm.json()},
                                           {"decoded_pcm", decoded.json()},
                                           {"lfe_pcm", lfe.json()},
                                           {"metrics", metric_comparison()},
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
