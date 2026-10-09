#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/render_triple_balance.h"
#include "adm/semantic_policy.h"

#include "render_state.h"

namespace {
void require(bool value, const std::string& label) {
    if (!value) {
        throw std::runtime_error(label);
    }
}
template <class T> T take(mradm::Result<T> value) {
    if (!value) {
        throw std::runtime_error(value.error().message + ": " + value.error().context);
    }
    return std::move(*value);
}
void done(mradm::Result<void> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
}
class Files {
  public:
    Files(std::filesystem::path input, std::filesystem::path output)
        : input_(std::move(input)), output_(std::move(output)) {}
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;
    Files(Files&&) = delete;
    Files& operator=(Files&&) = delete;
    ~Files() {
        std::error_code ec;
        std::filesystem::remove(input_, ec);
        std::filesystem::remove(output_, ec);
    }
    [[nodiscard]] const std::filesystem::path& input() const { return input_; }
    [[nodiscard]] const std::filesystem::path& output() const { return output_; }

  private:
    std::filesystem::path input_;
    std::filesystem::path output_;
};

mradm::RenderPlan make_plan(const Files& files, const std::string& layout) {
    mradm::RenderPlan plan;
    plan.input_path = files.input().string();
    plan.output_path = files.output().string();
    plan.output_layout = layout;
    plan.scene.info.sample_rate = 48000;
    plan.scene.info.num_channels = 13;
    plan.scene.info.num_frames = 100037;
    plan.scene.info.source_kind = mradm::SceneSourceKind::adm;
    mradm::SceneObject bed;
    bed.id = "bed";
    const std::array<std::string, 10> labels{
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
    for (uint16_t ch = 0; ch < 10; ++ch) {
        mradm::SceneTrackRef track;
        track.channel_index = ch;
        track.track_uid = "bed-" + std::to_string(ch);
        mradm::SceneDirectSpeakersBlock block;
        block.speaker_labels = {labels.at(ch)};
        track.ds_blocks.push_back(block);
        bed.tracks.push_back(track);
    }
    plan.scene.objects.push_back(bed);
    for (uint16_t ch = 10; ch < 13; ++ch) {
        mradm::SceneObject object;
        object.id = "object-" + std::to_string(ch);
        mradm::SceneTrackRef track;
        track.channel_index = ch;
        track.track_uid = object.id;
        for (uint64_t frame : {0U, 31U, 511U, 512U, 1025U, 1051U, 1126U, 16384U, 16391U, 66049U}) {
            mradm::SceneObjectBlock block;
            block.position.cartesian = true;
            block.position.x = frame == 0 ? -.4F : .3F;
            if (frame != 0 && frame % 3 == 0) {
                block.position.x = -.2F;
            }
            block.position.y = frame < 16384 ? .2F : -.3F;
            block.position.z = .5F;
            const float size = frame == 16384 ? .7F : .3F;
            block.width = block.height = block.depth = ch == 12 ? 0 : size;
            block.start_sample = frame;
            track.blocks.push_back(block);
        }
        object.tracks.push_back(track);
        plan.scene.objects.push_back(object);
    }
    return plan;
}

std::vector<float> offline(mradm::IRenderer& renderer, const mradm::RenderPlan& plan) {
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = take(renderer.prepare(plan, logs));
    take(renderer.render_window(*prepared, plan, progress, logs));
    auto reader = take(mradm::audio::FloatWavReader::open(plan.output_path));
    std::vector<float> samples(reader.frame_count() * reader.channels());
    require(reader.read(samples.data(), reader.frame_count()) == reader.frame_count(), "reference read");
    return samples;
}
std::vector<float> pull(mradm::IRenderStream& stream, std::size_t frames, std::span<const std::size_t> pattern) {
    std::vector<float> result;
    std::size_t index = 0;
    while (frames != 0) {
        const auto want = std::min(frames, pattern[index++ % pattern.size()]);
        std::vector<float> block(want * stream.out_channels());
        const std::size_t got = take(stream.process(std::span<float>(block.data(), block.size()), want));
        if (got == 0) {
            break;
        }
        result.insert(
            result.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got * stream.out_channels()));
        frames -= got;
    }
    return result;
}
void expect_equal(const std::vector<float>& actual, const std::vector<float>& expected, const std::string& label) {
    if (actual != expected) {
        auto mismatch = std::ranges::mismatch(actual, expected);
        std::cerr << label << " first difference at " << std::distance(actual.begin(), mismatch.in1) << '\n';
    }
    require(actual == expected, label);
}
void check_live_edits(const mradm::RenderPlan& plan,
                      mradm::IRenderer& renderer,
                      const mradm::IPreparedRender& prepared,
                      std::span<const std::size_t> chunks) {
    mradm::NullLogSink logs;
    auto solo = plan;
    solo.scene.objects = {plan.scene.objects.at(1)};
    auto solo_prepared = take(renderer.prepare(solo, logs));
    auto solo_stream = take(renderer.open_stream(*solo_prepared, solo, logs));
    const auto channels = std::size_t{solo_stream->out_channels()};
    const auto solo_reference = offline(renderer, solo);
    pull(*solo_stream, 4096, chunks);
    mradm::LiveObjectOverride mute;
    mute.object_id = "object-10";
    mute.mute = true;
    solo_stream->set_overrides({{mute}, 2});
    const auto muted = pull(*solo_stream, 2048, chunks);
    require(std::ranges::all_of(std::span{muted}.last(1024 * channels), [](float sample) { return sample == 0; }),
            "live mute reaches exact silence");
    solo_stream->set_overrides({});
    const auto resumed = pull(*solo_stream, 2048, chunks);
    expect_equal(std::vector<float>(resumed.end() - static_cast<std::ptrdiff_t>(1024 * channels), resumed.end()),
                 std::vector<float>(solo_reference.begin() + static_cast<std::ptrdiff_t>(uint64_t{7168} * channels),
                                    solo_reference.begin() + static_cast<std::ptrdiff_t>(uint64_t{8192} * channels)),
                 "mute/unmute never resets or rescales the decorrelator input");
    const auto automate = [&](std::span<const std::size_t> sizes) {
        auto edited = take(renderer.open_stream(prepared, plan, logs));
        std::vector<float> pcm;
        for (float scale : {1.0F, 0.0F, .5F, 4.0F, 1.0F}) {
            mradm::LiveObjectOverride edit;
            edit.object_id = "object-10";
            edit.extent_scale = scale;
            edited->set_overrides({{edit}, 3});
            auto block = pull(*edited, 4096, sizes);
            require(std::ranges::all_of(block, [](float sample) { return std::isfinite(sample); }),
                    "live size stays finite");
            pcm.insert(pcm.end(), block.begin(), block.end());
        }
        return pcm;
    };
    const std::array<std::size_t, 1> aligned{1024};
    expect_equal(automate(aligned), automate(chunks), "live size/point transitions are independent of pull sizes");
}

void run(const Files& files, const std::string& layout) {
    auto plan = make_plan(files, layout);
    auto renderer = mradm::create_triple_balance_renderer();
    mradm::NullLogSink logs;
    auto prepared = take(renderer->prepare(plan, logs));
    auto stream = take(renderer->open_stream(*prepared, plan, logs));
    const auto reference = offline(*renderer, plan);
    const std::array<std::size_t, 7> chunks{1, 7, 333, 512, 1000, 1024, 4097};
    expect_equal(pull(*stream, plan.scene.info.num_frames + 100, chunks), reference, layout + " stream/offline");
    const auto channels = stream->out_channels();
    for (uint64_t frame : {0U, 511U, 512U, 1023U, 1024U, 90001U, 16385U, 99999U, 100037U}) {
        done(stream->seek(frame));
        const auto count = std::min<uint64_t>(2049, plan.scene.info.num_frames - frame);
        expect_equal(pull(*stream, count + 1, chunks),
                     std::vector<float>(reference.begin() + static_cast<std::ptrdiff_t>(frame * channels),
                                        reference.begin() +
                                            static_cast<std::ptrdiff_t>(
                                                std::min(frame + count + 1, plan.scene.info.num_frames) * channels)),
                     layout + " exact seek");
    }
    std::stop_source cancel;
    cancel.request_stop();
    require(!stream->seek_with_cancel(95000, cancel.get_token()), "cancelled seek");
    require(take(stream->process({}, 0)) == 0, "empty process");
    require(!stream->process({}, 1), "short output buffer rejected");

    for (float scale : {0.0F, .5F, 1.0F, 4.0F}) {
        mradm::LiveObjectOverride object;
        object.object_id = "object-10";
        object.extent_scale = scale;
        object.gain_db = -6;
        mradm::LiveObjectOverride bed;
        bed.object_id = "bed";
        bed.speaker_label = "rc_l";
        bed.mute = true;
        mradm::LiveOverrides overrides{{object, bed}, 1};
        done(stream->validate_overrides(overrides));
        stream->set_overrides(overrides);
        done(stream->seek(0));
        auto effective = plan;
        const auto policy =
            take(mradm::parse_semantic_policy("{\"schema\":\"mradm.semantic-policy.v1\",\"objects\":[{\"id\":\"object-"
                                              "10\",\"gain\":{\"gain_db\":-6},\"extent\":{\"scale\":" +
                                                  std::to_string(scale) + "}}]}",
                                              "test"));
        done(mradm::apply_semantic_policy(effective.scene, policy, 48000));
        effective.scene.objects.front().tracks.front().ds_blocks.front().user_level.mute = true;
        const auto scaled = offline(*renderer, effective);
        expect_equal(pull(*stream, plan.scene.info.num_frames, chunks), scaled, layout + " static scale/gain policy");
        done(stream->seek(66050));
        expect_equal(pull(*stream, 3000, chunks),
                     std::vector<float>(scaled.begin() + static_cast<std::ptrdiff_t>(uint64_t{66050} * channels),
                                        scaled.begin() + static_cast<std::ptrdiff_t>(uint64_t{69050} * channels)),
                     layout + " scaled seek/checkpoint");
        auto invalid = overrides;
        invalid.objects.front().extent_width_scale = 2;
        if (scale != 0) {
            require(!stream->validate_overrides(invalid), "non-isotropic edit rejected");
            stream->set_overrides(invalid);
            done(stream->seek(66050));
            expect_equal(pull(*stream, 3000, chunks),
                         std::vector<float>(scaled.begin() + static_cast<std::ptrdiff_t>(uint64_t{66050} * channels),
                                            scaled.begin() + static_cast<std::ptrdiff_t>(uint64_t{69050} * channels)),
                         "rejection retains accepted target");
        }
    }
    stream->set_overrides({});
    done(stream->seek(0));
    expect_equal(pull(*stream, plan.scene.info.num_frames, chunks), reference, "clear restores baseline");
    // Exercise eviction and the no-cache fallback without allocating a large fixture.
    const auto& metadata = dynamic_cast<const mradm::triple_balance::Prepared&>(*prepared);
    for (const std::size_t budget :
         {std::size_t{0},
          take(mradm::triple_balance::SizeMixer::create(metadata, plan, true)).snapshot_bytes() * 3 / 2}) {
        auto limited = take(mradm::triple_balance::open_stream(metadata, plan, budget));
        pull(*limited, plan.scene.info.num_frames, chunks);
        for (uint64_t frame : {66050U, 1025U, 90001U, 66050U}) {
            done(limited->seek(frame));
            expect_equal(pull(*limited, 2048, chunks),
                         std::vector<float>(reference.begin() + static_cast<std::ptrdiff_t>(frame * channels),
                                            reference.begin() + static_cast<std::ptrdiff_t>((frame + 2048) * channels)),
                         "eviction and zero-budget seeks retain exact output");
        }
    }
    check_live_edits(plan, *renderer, *prepared, chunks);
    std::cout << layout << ": stream, policy, checkpoint and seek checks passed\n";
}
void check_d_lfe_routes(mradm::IRenderer& renderer, const mradm::RenderPlan& plan, std::span<const float> full) {
    auto direct_plan = plan;
    direct_plan.lfe_routing_mode = mradm::LfeRoutingMode::direct;
    const auto direct = offline(renderer, direct_plan);
    for (std::size_t i = 0; i < full.size(); i += 24) {
        require(direct[i + 9] == 0 && full[i + 3] == full[i + 9], "D 22.2 LFE channels share one signal");
        require(std::abs(full[i + 3] - (direct[i + 3] * std::sqrt(.5F))) < 1e-7F,
                "D 22.2 LFE uses equal-power gain including startup");
        for (std::size_t c = 0; c < 24; ++c) {
            if (c != 3 && c != 9) {
                require(full[i + c] == direct[i + c], "D 22.2 LFE routing preserves full-band PCM");
            }
        }
    }
}
void run_d(const Files& files, const std::string& layout) {
    auto plan = make_plan(files, layout);
    const bool extended = layout == "9+10+3";
    const std::size_t channels = extended ? 24U : 16U;
    plan.triple_balance_mode = mradm::TripleBalanceMode::d;
    if (extended) {
        plan.lfe_routing_mode = mradm::LfeRoutingMode::split_power;
        for (auto& object : plan.scene.objects) {
            for (auto& track : object.tracks) {
                for (auto& block : track.blocks) {
                    if (block.start_sample > 1024) {
                        block.position.z = -.5F;
                    }
                }
            }
        }
    }
    auto renderer = mradm::create_triple_balance_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    std::string report;
    plan.renderer_semantics_sink = [&](std::string value) { report = std::move(value); };
    auto prepared = take(renderer->prepare(plan, logs));
    const auto full = offline(*renderer, plan);
    require(full.size() == plan.scene.info.num_frames * channels, "D mode frame count");
    require(std::ranges::all_of(std::span{full}.first(channels), [](float x) { return x == 0; }),
            "D mode startup ramp");
    require(report.find(R"("mode":"d")") != std::string::npos &&
                report.find(R"("gain_control_frames":1536)") != std::string::npos,
            "D mode effective report");
    if (extended) {
        require(report.find("triple-balance-d-222-v1") != std::string::npos &&
                    report.find(R"("lfe_routing":"split-power")") != std::string::npos &&
                    report.find(R"("size_decorrelation":false)") != std::string::npos &&
                    report.find("target_mix_spread_gains") == std::string::npos,
                "D 22.2 report describes the effective coherent model");
        check_d_lfe_routes(*renderer, plan, full);
    }
    auto clipped = plan;
    clipped.render_window = mradm::RenderWindow{1537, 2051};
    take(renderer->render_window(*prepared, clipped, progress, logs));
    auto reader = take(mradm::audio::FloatWavReader::open(clipped.output_path));
    std::vector<float> actual(reader.frame_count() * reader.channels());
    require(reader.read(actual.data(), reader.frame_count()) == reader.frame_count(), "D mode clipped read");
    const auto expected = std::span{full}.subspan(1537 * channels, 2051 * channels);
    expect_equal(
        actual, std::vector<float>(expected.begin(), expected.end()), "D mode cropped output matches full PCM");
    expect_equal(offline(*renderer, plan), full, "D mode repeated prepared data");
    require(!renderer->open_stream(*prepared, plan, logs), "D mode realtime rejected");
    auto standard = plan;
    standard.triple_balance_mode = mradm::TripleBalanceMode::standard;
    require(!renderer->open_stream(*prepared, standard, logs), "D prepared state cannot enter standard stream");
    require(!renderer->render_window(*prepared, standard, progress, logs), "prepared mode mismatch rejected");
    auto wrong_layout = plan;
    wrong_layout.output_layout = extended ? "9.1.6" : "9+10+3";
    require(!renderer->render_window(*prepared, wrong_layout, progress, logs),
            "D mode prepared layout mismatch rejected");
    auto ignored = plan;
    ignored.speaker_spread_mode = mradm::SpeakerSpreadMode::none;
    auto zero = plan;
    for (auto& object : zero.scene.objects) {
        for (auto& track : object.tracks) {
            for (auto& block : track.blocks) {
                block.width = block.height = block.depth = block.diffuse = 0;
            }
        }
    }
    expect_equal(offline(*renderer, ignored), offline(*renderer, zero), "D mode spread none ignores size");
    for (const auto* rejected_layout : {"4+7+0", "0+5+0"}) {
        auto invalid = plan;
        invalid.output_layout = rejected_layout;
        auto result = renderer->prepare(invalid, logs);
        require(!result && result.error().code == mradm::ErrorCode::unsupported, "D mode layout boundary");
    }
    auto invalid = plan;
    invalid.scene.info.sample_rate = 44100;
    require(!renderer->prepare(invalid, logs), "D mode rate boundary");
    require(!renderer->render_window(*prepared, invalid, progress, logs), "D mode prepared rate mismatch rejected");
    std::cout << "D mode " << layout << ": offline, crop, repeat, LFE and unsupported-boundary checks passed\n";
}
} // namespace
int main() {
    try {
        const auto prefix =
            std::filesystem::temp_directory_path() /
            ("mr_triple_stream_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const Files files{prefix.string() + "-in.wav", prefix.string() + "-out.wav"};
        const auto plan = make_plan(files, "9.1.6");
        {
            auto writer = take(mradm::audio::FloatWavWriter::open(files.input().string(), 13, 48000));
            std::vector<float> samples(plan.scene.info.num_frames * 13);
            for (std::size_t i = 0; i < samples.size(); ++i) {
                samples[i] = .05F * std::sin(static_cast<float>(i) * .019F);
            }
            require(writer.write(samples.data(), plan.scene.info.num_frames) == plan.scene.info.num_frames,
                    "write input");
        }
        for (const auto* layout : {"4+7+0", "9.1.6", "9+10+3"}) {
            run(files, layout);
        }
        run_d(files, "9.1.6");
        run_d(files, "9+10+3");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
