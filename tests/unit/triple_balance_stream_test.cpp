#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/render_triple_balance.h"

#include "../adm_io/scene_import_internal.h"
#include "render_state.h"
#include "wav_fixture.h"

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
    // Removes the output and every variant input (paths starting with the input path).
    ~Files() {
        std::error_code ec;
        std::vector<std::filesystem::path> inputs;
        for (const auto& entry : std::filesystem::directory_iterator(input_.parent_path(), ec)) {
            if (entry.path().string().starts_with(input_.string())) {
                inputs.push_back(entry.path());
            }
        }
        for (const auto& path : inputs) {
            std::filesystem::remove(path, ec);
        }
        std::filesystem::remove(output_, ec);
    }
    [[nodiscard]] const std::filesystem::path& input() const { return input_; }
    [[nodiscard]] const std::filesystem::path& output() const { return output_; }

  private:
    std::filesystem::path input_;
    std::filesystem::path output_;
};

constexpr uint64_t k_frames = 100037;
constexpr uint16_t k_channels = 13;

// Variants of the fixture scene: object AO_1010 alone, the 22.2 lower layer after frame 1024,
// no Objects extent, another sample rate.
struct Variant {
    bool solo{false};
    bool lower{false};
    bool zero{false};
    uint32_t rate{48000};
};

std::string timecode(uint64_t frame, uint32_t rate) {
    const auto ns = frame * 1000000000U / rate;
    std::ostringstream out;
    out << "00:00:" << std::setfill('0') << std::setw(2) << ns / 1000000000U << '.' << std::setw(9) << ns % 1000000000U;
    return out.str();
}

std::string hex(int value, int width) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

// The RC 7.1.2 bed AO_1001 on channels 1-10 (the route ignores its Cartesian positions) and
// Objects AO_1010-AO_1012 on 11-13, with metadata events across control-block edges.
std::pair<std::string, std::vector<fixture::WaveAudioId>> scene_xml(const Variant& v) {
    const std::array<std::string, 10> labels{
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
    std::ostringstream formats;
    std::ostringstream uids;
    std::ostringstream objects;
    std::ostringstream top;
    formats.imbue(std::locale::classic());
    formats << std::setprecision(9);
    std::vector<fixture::WaveAudioId> chna;
    const auto track = [&](int n, const std::string& type, const std::string& blocks, const std::string& pack) {
        const auto code = type + hex(0x1000 + n, 4);
        const auto cf = "AC_" + code;
        const auto tf = "AT_" + code + "_01";
        const auto sf = "AS_" + code;
        const auto uid = "ATU_" + hex(n, 8);
        formats << R"(<audioChannelFormat audioChannelFormatID=")" << cf << R"(" audioChannelFormatName="CF)" << n
                << R"(" typeLabel=")" << type << R"(">)" << blocks << "</audioChannelFormat>"
                << R"(<audioStreamFormat audioStreamFormatID=")" << sf << R"(" audioStreamFormatName="SF)" << n
                << R"(" formatDefinition="pcm" formatLabel="0001"><audioChannelFormatIDRef>)" << cf
                << "</audioChannelFormatIDRef><audioTrackFormatIDRef>" << tf
                << "</audioTrackFormatIDRef></audioStreamFormat>" << R"(<audioTrackFormat audioTrackFormatID=")" << tf
                << R"(" audioTrackFormatName="TF)" << n
                << R"(" formatDefinition="pcm" formatLabel="0001"><audioStreamFormatIDRef>)" << sf
                << "</audioStreamFormatIDRef></audioTrackFormat>";
        uids << R"(<audioTrackUID UID=")" << uid << R"("><audioTrackFormatIDRef>)" << tf
             << "</audioTrackFormatIDRef><audioPackFormatIDRef>" << pack << "</audioPackFormatIDRef></audioTrackUID>";
        chna.emplace_back(static_cast<uint16_t>(n), uid, tf, pack);
        return std::pair{cf, uid};
    };
    if (!v.solo) {
        std::string refs;
        std::string bed_uids;
        for (int ch = 1; ch <= 10; ++ch) {
            const auto block =
                R"(<audioBlockFormat audioBlockFormatID="AB_0001)" + std::to_string(1000 + ch) +
                R"(_00000001"><position coordinate="X">0</position><position coordinate="Y">1</position>)"
                R"(<position coordinate="Z">0</position><cartesian>1</cartesian><speakerLabel>)" +
                labels.at(ch - 1) + "</speakerLabel></audioBlockFormat>";
            const auto [cf, uid] = track(ch, "0001", block, "AP_00011000");
            refs += "<audioChannelFormatIDRef>" + cf + "</audioChannelFormatIDRef>";
            bed_uids += "<audioTrackUIDRef>" + uid + "</audioTrackUIDRef>";
        }
        formats << R"(<audioPackFormat audioPackFormatID="AP_00011000" audioPackFormatName="Bed" typeLabel="0001">)"
                << refs << "</audioPackFormat>";
        objects << R"(<audioObject audioObjectID="AO_1001" audioObjectName="bed">)" << bed_uids << "</audioObject>";
        top << "<audioObjectIDRef>AO_1001</audioObjectIDRef>";
    }
    for (int ch = 11; ch <= 13; ++ch) {
        if (v.solo && ch != 11) {
            continue;
        }
        std::ostringstream blocks;
        blocks.imbue(std::locale::classic());
        blocks << std::setprecision(9);
        int index = 0;
        for (uint64_t frame : {0U, 31U, 511U, 512U, 1025U, 1051U, 1126U, 16384U, 16391U, 66049U}) {
            float x = frame == 0 ? -.4F : .3F;
            if (frame != 0 && frame % 3 == 0) {
                x = -.2F;
            }
            const float y = frame < 16384 ? .2F : -.3F;
            const float z = v.lower && frame > 1024 ? -.5F : .5F;
            float size = frame == 16384 ? .7F : .3F;
            if (v.zero || ch == 13) {
                size = 0;
            }
            blocks << R"(<audioBlockFormat audioBlockFormatID="AB_0003)" << 1000 + ch << '_' << std::setfill('0')
                   << std::setw(8) << ++index << R"(" rtime=")" << timecode(frame, v.rate)
                   << R"("><position coordinate="X">)" << x << R"(</position><position coordinate="Y">)" << y
                   << R"(</position><position coordinate="Z">)" << z << "</position><cartesian>1</cartesian><width>"
                   << size << "</width><height>" << size << "</height><depth>" << size << "</depth></audioBlockFormat>";
        }
        const auto pack = "AP_0003" + std::to_string(1000 + ch);
        const auto [cf, uid] = track(ch, "0003", blocks.str(), pack);
        formats << R"(<audioPackFormat audioPackFormatID=")" << pack << R"(" audioPackFormatName="PF)" << ch
                << R"(" typeLabel="0003"><audioChannelFormatIDRef>)" << cf
                << "</audioChannelFormatIDRef></audioPackFormat>";
        const auto id = "AO_10" + std::to_string(ch - 1);
        objects << R"(<audioObject audioObjectID=")" << id << R"(" audioObjectName="object-)" << ch - 1
                << R"("><audioTrackUIDRef>)" << uid << "</audioTrackUIDRef></audioObject>";
        top << "<audioObjectIDRef>" << id << "</audioObjectIDRef>";
    }
    std::ostringstream xml;
    xml << R"(<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain><coreMetadata><format><audioFormatExtended>)"
        << formats.str() << uids.str() << objects.str()
        << R"(<audioContent audioContentID="ACO_1001" audioContentName="C">)" << top.str()
        << R"(</audioContent><audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="P">)"
        << "<audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme></audioFormatExtended></format>"
        << "</coreMetadata></ebuCoreMain>";
    return {xml.str(), chna};
}

// The input of a variant (the same PCM for every one) and the plan of its imported Rust scene,
// with `policy` applied as RenderService does.
mradm::RenderPlan
make_plan(const Files& files, const std::string& layout, const Variant& v = {}, const std::string& policy = {}) {
    const auto name = files.input().string() + (v.solo ? "-solo" : "") + (v.lower ? "-lower" : "") +
                      (v.zero ? "-zero" : "") + "-" + std::to_string(v.rate) + ".wav";
    if (!std::filesystem::exists(name)) {
        auto [xml, chna] = scene_xml(v);
        auto writer = fixture::write_wave(name,
                                          k_channels,
                                          v.rate,
                                          24,
                                          std::make_shared<fixture::WaveChna>(std::move(chna)),
                                          std::make_shared<fixture::WaveAxml>(std::move(xml)));
        std::vector<float> samples(k_frames * k_channels);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = .05F * std::sin(static_cast<float>(i) * .019F);
        }
        writer->write(samples.data(), k_frames);
    }
    auto scene = take(mradm::io::RenderScene::import(name, nullptr));
    if (!policy.empty()) {
        done(scene.apply_policy(policy, "<memory>"));
    }
    mradm::RenderPlan plan;
    plan.input_path = name;
    plan.output_path = files.output().string();
    plan.output_layout = layout;
    plan.scene = scene.scene();
    plan.scene_handle = scene.handle();
    return plan;
}

// The offline reference of the live object edit and the RC_L bed mute.
std::string object_policy(float scale) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << R"({"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1010","gain":{"gain_db":-6},"extent":{"scale":)"
        << scale << R"(}},{"id":"AO_1001","direct_speakers":{"speaker_label":"RC_L","gain":{"mute":true}}}]})";
    return out.str();
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
void check_live_edits(const Files& files,
                      const mradm::RenderPlan& plan,
                      mradm::IRenderer& renderer,
                      const mradm::IPreparedRender& prepared,
                      std::span<const std::size_t> chunks) {
    mradm::NullLogSink logs;
    auto solo = make_plan(files, plan.output_layout, {.solo = true});
    auto solo_prepared = take(renderer.prepare(solo, logs));
    auto solo_stream = take(renderer.open_stream(*solo_prepared, solo, logs));
    const auto channels = std::size_t{solo_stream->out_channels()};
    const auto solo_reference = offline(renderer, solo);
    pull(*solo_stream, 4096, chunks);
    mradm::LiveObjectOverride mute;
    mute.object_id = "AO_1010";
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
            edit.object_id = "AO_1010";
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
        object.object_id = "AO_1010";
        object.extent_scale = scale;
        object.gain_db = -6;
        mradm::LiveObjectOverride bed;
        bed.object_id = "AO_1001";
        bed.speaker_label = "rc_l";
        bed.mute = true;
        mradm::LiveOverrides overrides{{object, bed}, 1};
        done(stream->validate_overrides(overrides));
        stream->set_overrides(overrides);
        done(stream->seek(0));
        const auto effective = make_plan(files, layout, {}, object_policy(scale));
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
    check_live_edits(files, plan, *renderer, *prepared, chunks);
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
    const bool extended = layout == "9+10+3";
    auto plan = make_plan(files, layout, {.lower = extended});
    const std::size_t channels = extended ? 24U : 16U;
    plan.triple_balance_mode = mradm::TripleBalanceMode::d;
    if (extended) {
        plan.lfe_routing_mode = mradm::LfeRoutingMode::split_power;
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
    auto zero = make_plan(files, layout, {.lower = extended, .zero = true});
    zero.triple_balance_mode = plan.triple_balance_mode;
    zero.lfe_routing_mode = plan.lfe_routing_mode;
    expect_equal(offline(*renderer, ignored), offline(*renderer, zero), "D mode spread none ignores size");
    for (const auto* rejected_layout : {"4+7+0", "0+5+0"}) {
        auto invalid = plan;
        invalid.output_layout = rejected_layout;
        auto result = renderer->prepare(invalid, logs);
        require(!result && result.error().code == mradm::ErrorCode::unsupported, "D mode layout boundary");
    }
    auto invalid = make_plan(files, layout, {.lower = extended, .rate = 44100});
    invalid.triple_balance_mode = plan.triple_balance_mode;
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
