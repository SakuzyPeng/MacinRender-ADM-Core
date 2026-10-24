#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <sys/resource.h>
#endif

#include "adm/audio_io.h"
#include "adm/render_triple_balance.h"

#include "../adm_audio/audio_io_internal.h"
#include "../adm_io/scene_import_internal.h"
#include "adm_fixture.h"
#include "audio_output_device.h"
#include "monitor_engine.h"
#include "render_stream_factory.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t k_sample_rate = 48000;
constexpr uint16_t k_input_channels = 13;
constexpr uint64_t k_frames = uint64_t{k_sample_rate} * 70;
class Factory final : public mradm::realtime::IRenderStreamFactory {
  public:
    Factory(mradm::IRenderer& renderer, const mradm::IPreparedRender& prepared, const mradm::RenderPlan& plan)
        : renderer_(renderer), prepared_(prepared), plan_(plan) {}
    mradm::Result<std::unique_ptr<mradm::IRenderStream>>
    open(const mradm::AdmScene& /*scene*/, const mradm::RenderOptions& /*options*/, mradm::LogSink& logs) override {
        return renderer_.open_stream(prepared_, plan_, logs);
    }

  private:
    mradm::IRenderer& renderer_;
    const mradm::IPreparedRender& prepared_;
    const mradm::RenderPlan& plan_;
};
class InputFile {
  public:
    explicit InputFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~InputFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;
    InputFile(InputFile&&) = delete;
    InputFile& operator=(InputFile&&) = delete;

  private:
    std::filesystem::path path_;
};
[[maybe_unused]] double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Author the same bed and motion as the original benchmark, then import them through the
// production ADM path so both the public scene and its Rust handle describe the input file.
mradm::audio::WavLayoutFinalization fixture_metadata() {
    auto doc = fixture::Document::create();
    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"benchmark"});
    auto content = fixture::AudioContent::create(fixture::AudioContentName{"benchmark"});
    doc->add(programme);
    programme->add_reference(content);
    auto bed = fixture::AudioObject::create(fixture::AudioObjectName{"bed"});
    auto bed_pack =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"bed"}, fixture::TypeDefinition::direct_speakers);
    bed->add_reference(bed_pack);
    std::vector<std::shared_ptr<fixture::AudioTrackUid>> uids;
    const std::array<std::string, 10> labels{
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
    for (uint16_t channel = 0; channel < k_input_channels; ++channel) {
        const auto name = "track-" + std::to_string(channel);
        const auto type = channel < 10 ? fixture::TypeDefinition::direct_speakers : fixture::TypeDefinition::objects;
        auto format = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{name}, type);
        auto pack =
            channel < 10 ? bed_pack : fixture::AudioPackFormat::create(fixture::AudioPackFormatName{name}, type);
        auto object = channel < 10 ? bed : fixture::AudioObject::create(fixture::AudioObjectName{name});
        if (channel < 10) {
            format->add(fixture::AudioBlockFormatDirectSpeakers{fixture::CartesianSpeakerPosition{},
                                                                fixture::SpeakerLabel{labels.at(channel)}});
        } else {
            for (uint64_t frame = 0; frame < k_frames; frame += k_sample_rate) {
                const float x = .6F * std::sin(static_cast<float>(frame) / k_sample_rate);
                const float y = .6F * std::cos(static_cast<float>(frame) / k_sample_rate);
                const float size = channel == 12 ? 0 : .3F;
                format->add(fixture::AudioBlockFormatObjects{
                    fixture::CartesianPosition{fixture::X{x}, fixture::Y{y}, fixture::Z{.5F}},
                    fixture::Cartesian{true},
                    fixture::Rtime{fixture::Time{std::chrono::seconds{frame / k_sample_rate}}},
                    fixture::Width{size},
                    fixture::Height{size},
                    fixture::Depth{size}});
            }
            object->add_reference(pack);
            content->add_reference(object);
        }
        pack->add_reference(format);
        auto stream =
            fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{name}, fixture::FormatDefinition::pcm);
        auto track =
            fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{name}, fixture::FormatDefinition::pcm);
        auto uid = fixture::AudioTrackUid::create();
        doc->add(uid);
        uid->set_reference(track);
        uid->set_reference(pack);
        track->set_reference(stream);
        stream->set_reference(format);
        stream->add_reference(track);
        object->add_reference(uid);
        uids.push_back(std::move(uid));
    }
    content->add_reference(bed);
    fixture::reassign_ids(doc);
    std::ostringstream xml;
    fixture::write_xml(xml, doc);
    mradm::audio::WavLayoutFinalization metadata;
    metadata.axml = xml.str();
    for (uint16_t channel = 0; channel < k_input_channels; ++channel) {
        const auto& uid = uids.at(channel);
        metadata.chna.push_back({static_cast<uint16_t>(channel + 1U),
                                 uid->id,
                                 uid->get_references<fixture::AudioTrackFormat>().front()->id,
                                 uid->get_references<fixture::AudioPackFormat>().front()->id});
    }
    return metadata;
}

[[maybe_unused]] mradm::Result<mradm::RenderPlan> make_plan(const std::string& input, const std::string& layout) {
    {
        auto writer = mradm::audio::FloatWavWriter::open(input, k_input_channels, k_sample_rate);
        if (!writer) {
            return tl::unexpected{writer.error()};
        }
        std::vector<float> pcm(std::size_t{1024} * k_input_channels);
        for (uint64_t start = 0; start < k_frames; start += 1024) {
            const auto frames = std::min<uint64_t>(1024, k_frames - start);
            for (std::size_t f = 0; f < frames; ++f) {
                for (std::size_t ch = 0; ch < k_input_channels; ++ch) {
                    pcm[(f * k_input_channels) + ch] =
                        .025F * std::sin(static_cast<float>(start + f) * (.017F + .002F * static_cast<float>(ch)));
                }
            }
            if (writer->write(pcm.data(), frames) != frames) {
                return mradm::make_error(mradm::ErrorCode::io_error, "short benchmark input write");
            }
        }
        if (auto finished = writer->finish(); !finished) {
            return tl::unexpected{finished.error()};
        }
    }
    if (auto finalized = mradm::audio::finalize_wav_layout(input, fixture_metadata()); !finalized) {
        return tl::unexpected{finalized.error()};
    }
    auto scene = mradm::io::RenderScene::import(input, nullptr);
    if (!scene) {
        return tl::unexpected{scene.error()};
    }
    mradm::RenderPlan plan;
    plan.input_path = input;
    plan.output_layout = layout;
    plan.scene = scene->scene();
    plan.scene_handle = scene->handle();
    return plan;
}
} // namespace

// NOLINTNEXTLINE(readability-function-size): keep fixture lifetime and measured intervals together.
int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
#ifndef NDEBUG
    std::cerr << "Run this benchmark from the Release build.\n";
    return 2;
#else
    const std::string layout = argc > 1 ? argv[1] : "9.1.6";
    const auto path = std::filesystem::temp_directory_path() /
                      ("mr_triple_benchmark_" + std::to_string(Clock::now().time_since_epoch().count()) + ".wav");
    const InputFile guard(path);
    const auto plan_result = make_plan(path.string(), layout);
    if (!plan_result) {
        std::cerr << plan_result.error().message << '\n';
        return 1;
    }
    const auto& plan = *plan_result;
    mradm::NullLogSink logs;
    auto renderer = mradm::create_triple_balance_renderer();
    auto prepared = renderer->prepare(plan, logs);
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return 1;
    }
    auto stream = renderer->open_stream(**prepared, plan, logs);
    if (!stream) {
        return 1;
    }
    std::vector<float> scratch(std::size_t{1024} * (*stream)->out_channels());
    auto start = Clock::now();
    while (true) {
        auto count = (*stream)->process(scratch, 1024);
        if (!count) {
            return 1;
        }
        if (*count == 0) {
            break;
        }
    }
    const double rtf = elapsed(start) / 70;
    stream = renderer->open_stream(**prepared, plan, logs);
    if (!stream) {
        return 1;
    }
    start = Clock::now();
    if (!(*stream)->seek((uint64_t{40} * 48000) + 7)) {
        return 1;
    }
    const double cold_seek = elapsed(start) * 1000;
    start = Clock::now();
    if (!(*stream)->seek((uint64_t{40} * 48000) + 7)) {
        return 1;
    }
    const double warm_seek = elapsed(start) * 1000;
    stream->reset();
    std::cout << "layout=" << layout << " inputs=13 size_objects=2 point_objects=1 bed=7.1.2 RTF=" << rtf
              << " cold_seek_ms=" << cold_seek << " cached_seek_ms=" << warm_seek << '\n'
              << std::flush;
    Factory factory(*renderer, **prepared, plan);
    auto device = mradm::realtime::make_miniaudio_device(true);
    auto engine = mradm::realtime::MonitorEngine::create(factory, *device, plan.scene, {}, logs);
    if (!engine) {
        std::cerr << engine.error().message << '\n';
        return 1;
    }
    (*engine)->set_loop((uint64_t{4} * 48000) + 7, (uint64_t{8} * 48000) + 11);
    (*engine)->play();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto before = (*engine)->status().underruns;
    start = Clock::now();
    for (unsigned seconds = 0; seconds < 60; ++seconds) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if ((seconds + 1) % 15 == 0) {
            std::cout << "monitor_seconds=" << seconds + 1 << " underruns=" << (*engine)->status().underruns - before
                      << '\n'
                      << std::flush;
        }
    }
    const auto status = (*engine)->status();
    std::cout << "elapsed_s=" << elapsed(start) << " underruns=" << status.underruns - before
              << " failed=" << status.failed;
#ifdef __APPLE__
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << " peak_rss_MiB=" << static_cast<double>(usage.ru_maxrss) / (1024 * 1024);
#endif
    std::cout << '\n' << std::flush;
    return rtf < 1 && !status.failed && status.underruns == before ? 0 : 1;
#endif
}
