// Blocks of one track sharing a start (PCM consistency S08): the last block in scene (ADM document)
// order is in effect for its whole interval. Overwritten blocks must leave nothing behind: no
// interpolation predecessor, no lane resuming after a shorter winner ends, and no LFE routing
// decision. Each case renders a track with 40 blocks at each of two starts and compares the PCM
// bit for bit against the same track holding only the winning blocks. Binaural PCM is covered by
// binaural_render_fixture_test; its live label binding is checked here with the other backends.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/live_override.h"
#include "adm/render.h"
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

#include "../support/adm_fixture.h"
#include "../support/wav_fixture.h"

namespace {

constexpr uint32_t k_frames = 4096U;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
    }
    return condition;
}

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() { std::filesystem::remove(path_); }

  private:
    std::filesystem::path path_;
};

// One Objects track with a single placeholder block; the test replaces the scene's blocks.
std::filesystem::path write_input() {
    auto doc = fixture::Document::create();
    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"SameStartCF"},
                                                  fixture::TypeDefinition::objects);
    fixture::AudioBlockFormatObjects block{
        fixture::SphericalPosition{fixture::Azimuth{0.0F}, fixture::Elevation{0.0F}}};
    block.set(fixture::Gain{1.0F});
    cf->add(block);
    doc->add(cf);
    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"SameStartPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);
    auto sf = fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"SameStartSF"},
                                                 fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);
    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"SameStartTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);
    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);
    auto obj = fixture::AudioObject::create(fixture::AudioObjectName{"SameStartObject"});
    obj->add_reference(uid);
    doc->add(obj);
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"SameStartContent"});
    content->add_reference(obj);
    doc->add(content);
    auto prog = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"SameStartProgramme"});
    prog->add_reference(content);
    doc->add(prog);
    fixture::reassign_ids(doc);

    std::ostringstream xml;
    fixture::write_xml(xml, doc);
    auto chna = std::make_shared<fixture::WaveChna>(std::vector<fixture::WaveAudioId>{
        fixture::WaveAudioId(1U, fixture::format_id(uid->get<fixture::AudioTrackUidId>()), "", "")});
    auto axml = std::make_shared<fixture::WaveAxml>(xml.str());
    const auto path = std::filesystem::temp_directory_path() / "mr_same_start_blocks_input.wav";
    auto writer = fixture::write_wave(path.string(), 1U, 48000U, 24U, chna, axml);
    std::vector<float> samples(k_frames);
    for (uint32_t i = 0; i < k_frames; ++i) {
        samples[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(i) / 48000.0F);
    }
    writer->write(samples.data(), k_frames);
    return path;
}

enum class Kind : uint8_t { objects, direct_speakers };

// Two starts written in reverse chronological order. Each winner is shorter than its overwritten
// peers and interpolates from its predecessor, so a peer would show up either as the ramp origin
// or by resuming after the winner ends.
void set_blocks(mradm::SceneTrackRef& track, Kind kind, bool include_overwritten) {
    track.blocks.clear();
    track.ds_blocks.clear();
    for (int i = include_overwritten ? 0 : 39; i < 40; ++i) {
        for (int group = 1; group >= 0; --group) {
            const bool winner = i == 39;
            const uint64_t start = static_cast<uint64_t>(group) * 2048U;
            const uint64_t end = start + (winner ? 1024U : 2048U);
            const float winning_azimuth = group == 0 ? 97.5F : -37.5F;
            const float azimuth = winner ? winning_azimuth : -97.5F + (5.0F * static_cast<float>(i));
            if (kind == Kind::direct_speakers) {
                mradm::SceneDirectSpeakersBlock block;
                block.start_sample = start;
                block.end_sample = end;
                block.azimuth = azimuth;
                block.has_position = true;
                block.gain = winner ? 0.375F : 0.9F;
                if (!winner) {
                    // An overwritten LFE1 next to a winning LFE2 must not make 22.2 split-power
                    // routing see two semantic LFE channels.
                    block.low_pass_hz = 120.0F;
                } else if (group == 0) {
                    block.speaker_labels = {"LFE2"};
                }
                track.ds_blocks.push_back(block);
                continue;
            }
            mradm::SceneObjectBlock block;
            block.start_sample = start;
            block.end_sample = end;
            block.position.azimuth = azimuth;
            block.position.elevation = winner ? 0.0F : 30.0F;
            block.gain = winner ? 0.375F : 0.9F;
            if (!winner) {
                block.width = 40.0F;
                block.height = 20.0F;
                block.diffuse = 0.25F;
                block.divergence = 0.5F;
                block.divergence_azimuth_range = 30.0F;
                block.jump_position = i % 2 == 0;
            }
            track.blocks.push_back(block);
        }
    }
}

struct Output {
    std::vector<float> offline;
    std::vector<float> streaming;
};

std::optional<Output> render(const mradm::RenderPlan& base,
                             mradm::IRenderer& renderer,
                             std::string_view label,
                             const mradm::LiveOverrides* overrides = nullptr) {
    const auto path = std::filesystem::temp_directory_path() / "mr_same_start_blocks_output.wav";
    FileGuard guard(path);
    mradm::RenderPlan plan = base;
    plan.output_path = path.string();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer.prepare(plan, logs);
    if (!check(prepared.has_value(), std::string{label} + ": prepare") ||
        !check(renderer.render_window(**prepared, plan, progress, logs).has_value(),
               std::string{label} + ": offline rendering")) {
        return std::nullopt;
    }
    auto reader = mradm::audio::FloatWavReader::open(path.string());
    if (!check(reader && reader->frame_count() == k_frames, std::string{label} + ": complete output")) {
        return std::nullopt;
    }
    const auto channels = static_cast<std::size_t>(reader->channels());
    Output output;
    output.offline.resize(static_cast<std::size_t>(reader->frame_count()) * channels);
    reader->read(output.offline.data(), reader->frame_count());

    auto stream = renderer.open_stream(**prepared, plan, logs);
    if (!check(stream.has_value(), std::string{label} + ": file streaming opens")) {
        return std::nullopt;
    }
    if (overrides != nullptr) {
        (*stream)->set_overrides(*overrides);
    }
    const std::array<std::size_t, 4> chunks{7U, 333U, 1024U, 19U};
    std::size_t chunk = 0;
    for (std::size_t remaining = k_frames; remaining > 0;) {
        const auto want = std::min(remaining, chunks.at(chunk++ % chunks.size()));
        std::vector<float> block(want * channels);
        const auto got = (*stream)->process(block, want);
        if (!check(got.has_value(), std::string{label} + ": file streaming pull")) {
            return std::nullopt;
        }
        if (*got == 0U) {
            break;
        }
        output.streaming.insert(
            output.streaming.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*got * channels));
        remaining -= *got;
    }
    if (!check(output.streaming.size() == output.offline.size(), std::string{label} + ": complete file stream")) {
        return std::nullopt;
    }
    return output;
}

bool bit_equal(std::span<const float> actual, std::span<const float> expected) {
    return std::ranges::equal(
        actual, expected, [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); });
}

bool nonzero(std::span<const float> samples) {
    return std::ranges::any_of(samples, [](float v) { return v != 0.0F; });
}

struct Backend {
    std::string_view name;
    std::unique_ptr<mradm::IRenderer> (*create)(){nullptr};
    std::string_view layout;
    Kind kind{Kind::objects};
    mradm::LfeRoutingMode lfe_mode{mradm::LfeRoutingMode::direct};
};

bool verify_live_label(mradm::RenderPlan plan, const Backend& backend) {
    auto& track = plan.scene.objects.front().tracks.front();
    track.blocks.clear();
    plan.lfe_routing_mode = mradm::LfeRoutingMode::direct;
    bool ok = true;
    for (const std::string_view winning_label : {"M+030", ""}) {
        mradm::SceneDirectSpeakersBlock first;
        first.end_sample = 2048U;
        first.has_position = true;
        first.azimuth = 30.0F;
        if (!winning_label.empty()) {
            first.speaker_labels = {std::string{winning_label}};
        }
        auto second = first;
        second.start_sample = first.end_sample;
        second.end_sample = k_frames;
        auto overwritten_first = first;
        overwritten_first.speaker_labels = {"M-030"};
        overwritten_first.azimuth = -30.0F;
        auto overwritten_second = overwritten_first;
        overwritten_second.start_sample = second.start_sample;
        overwritten_second.end_sample = k_frames;

        for (const std::string_view override_label : {"M+030", "M-030", ""}) {
            for (const bool mute : {false, true}) {
                mradm::LiveObjectOverride item;
                item.object_id = plan.scene.objects.front().id;
                item.speaker_label = override_label;
                item.gain_db = -12.0F;
                item.mute = mute;
                const mradm::LiveOverrides overrides{{item}, 1U};
                const std::string label = std::string{backend.name} + " live label='" + std::string{winning_label} +
                                          "' override='" + std::string{override_label} + "' mute=" + (mute ? "1" : "0");
                track.ds_blocks = {first, second};
                const auto expected = render(plan, *backend.create(), label + " (winners only)", &overrides);
                // Reverse chronological order, with a stale label at both starts. An unlabeled
                // winner must also discard the overwritten label and retain whole-object controls.
                track.ds_blocks = {overwritten_second, overwritten_first, second, first};
                const auto actual = render(plan, *backend.create(), label, &overrides);
                if (!actual || !expected) {
                    ok = false;
                    continue;
                }
                const bool should_mute = mute && (override_label.empty() || override_label == winning_label);
                ok &= check(nonzero(expected->offline), label + ": unmodified output has signal");
                ok &= check(nonzero(expected->streaming) != should_mute, label + ": override targets the winner");
                ok &= check(bit_equal(actual->streaming, expected->streaming),
                            label + ": file-streaming override equals the last blocks");
            }
        }
    }
    return ok;
}

} // namespace

int main() try {
    const auto input = write_input();
    FileGuard input_guard(input);
    const auto scene = mradm::io::import_scene(input.string());
    if (!check(scene && scene->objects.size() == 1U && scene->objects.front().tracks.size() == 1U, "input scene")) {
        return 1;
    }
    const std::array backends{
        Backend{"ear", mradm::create_ear_renderer, "4+5+0", Kind::objects, mradm::LfeRoutingMode::direct},
        Backend{"ear", mradm::create_ear_renderer, "9+10+3", Kind::direct_speakers, mradm::LfeRoutingMode::split_power},
        Backend{"vbap", mradm::create_vbap_renderer, "4+5+0", Kind::objects, mradm::LfeRoutingMode::direct},
        Backend{
            "vbap", mradm::create_vbap_renderer, "9+10+3", Kind::direct_speakers, mradm::LfeRoutingMode::split_power},
        Backend{"hoa", mradm::create_hoa_renderer, "hoa3", Kind::objects, mradm::LfeRoutingMode::direct},
        Backend{"hoa", mradm::create_hoa_renderer, "hoa3", Kind::direct_speakers, mradm::LfeRoutingMode::direct},
    };
    bool ok = true;
    {
        // Binaural PCM is covered by binaural_render_fixture_test; this adds its live label binding.
        const Backend binaural{"binaural",
                               mradm::create_binaural_renderer,
                               "binaural",
                               Kind::direct_speakers,
                               mradm::LfeRoutingMode::direct};
        mradm::RenderPlan plan;
        plan.input_path = input.string();
        plan.output_layout = std::string{binaural.layout};
        plan.scene = *scene;
        ok &= verify_live_label(std::move(plan), binaural);
    }
    for (const auto& backend : backends) {
        if (backend.kind == Kind::direct_speakers) {
            mradm::RenderPlan plan;
            plan.input_path = input.string();
            plan.output_layout = std::string{backend.layout};
            plan.scene = *scene;
            ok &= verify_live_label(std::move(plan), backend);
        }
        for (const uint32_t smoothing : {0U, 1024U}) {
            const std::string label = std::string{backend.name} + " " + std::string{backend.layout} +
                                      (backend.kind == Kind::objects ? " objects" : " direct-speakers") +
                                      " smoothing=" + std::to_string(smoothing);
            mradm::RenderPlan plan;
            plan.input_path = input.string();
            plan.output_layout = std::string{backend.layout};
            plan.scene = *scene;
            plan.object_smoothing_frames = smoothing;
            plan.lfe_routing_mode = backend.lfe_mode;
            auto& track = plan.scene.objects.front().tracks.front();
            set_blocks(track, backend.kind, false);
            const auto expected = render(plan, *backend.create(), label + " (winners only)");
            set_blocks(track, backend.kind, true);
            const auto actual = render(plan, *backend.create(), label);
            if (!actual || !expected) {
                ok = false;
                continue;
            }
            ok &= check(nonzero(expected->offline), label + ": winners produce output");
            ok &= check(bit_equal(actual->offline, expected->offline), label + ": offline PCM equals the last blocks");
            ok &= check(bit_equal(actual->streaming, expected->streaming),
                        label + ": file-streaming PCM equals the last blocks");
        }
    }
    if (!ok) {
        return 1;
    }
    std::cout << "same-start block tests passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: exception: " << e.what() << "\n";
    return 1;
}
