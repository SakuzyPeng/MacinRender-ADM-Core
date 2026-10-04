#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <sys/resource.h>
#endif

#include "adm/audio_io.h"
#include "adm/render_triple_balance.h"

#include "audio_output_device.h"
#include "monitor_engine.h"
#include "render_stream_factory.h"

namespace {
using Clock = std::chrono::steady_clock;
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

[[maybe_unused]] mradm::RenderPlan make_plan(const std::string& input, const std::string& layout) {
    mradm::RenderPlan plan;
    plan.input_path = input;
    plan.output_layout = layout;
    plan.scene.info.sample_rate = 48000;
    plan.scene.info.num_channels = 13;
    plan.scene.info.num_frames = uint64_t{48000} * 70;
    plan.scene.info.source_kind = mradm::SceneSourceKind::adm;
    mradm::SceneObject bed;
    bed.id = "bed";
    const std::array<std::string, 10> labels{
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
    for (uint16_t channel = 0; channel < 13; ++channel) {
        mradm::SceneTrackRef track;
        track.channel_index = channel;
        track.track_uid = "track-" + std::to_string(channel);
        if (channel < 10) {
            mradm::SceneDirectSpeakersBlock block;
            block.speaker_labels = {labels.at(channel)};
            track.ds_blocks.push_back(block);
            bed.tracks.push_back(track);
        } else {
            mradm::SceneObject object;
            object.id = "object-" + std::to_string(channel);
            for (uint64_t frame = 0; frame < plan.scene.info.num_frames; frame += 48000) {
                mradm::SceneObjectBlock block;
                block.start_sample = frame;
                block.position.cartesian = true;
                block.position.x = .6F * std::sin(static_cast<float>(frame) / 48000);
                block.position.y = .6F * std::cos(static_cast<float>(frame) / 48000);
                block.position.z = .5F;
                block.width = block.height = block.depth = channel == 12 ? 0 : .3F;
                track.blocks.push_back(block);
            }
            object.tracks.push_back(track);
            plan.scene.objects.push_back(object);
        }
    }
    plan.scene.objects.push_back(bed);
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
    const auto plan = make_plan(path.string(), layout);
    {
        auto writer = mradm::audio::FloatWavWriter::open(plan.input_path, 13, 48000);
        if (!writer) {
            return 1;
        }
        std::vector<float> pcm(std::size_t{1024} * 13);
        for (uint64_t start = 0; start < plan.scene.info.num_frames; start += 1024) {
            const auto frames = std::min<uint64_t>(1024, plan.scene.info.num_frames - start);
            for (std::size_t f = 0; f < frames; ++f) {
                for (std::size_t ch = 0; ch < 13; ++ch) {
                    pcm[(f * 13) + ch] =
                        .025F * std::sin(static_cast<float>(start + f) * (.017F + .002F * static_cast<float>(ch)));
                }
            }
            if (writer->write(pcm.data(), frames) != frames) {
                return 1;
            }
        }
    }
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
