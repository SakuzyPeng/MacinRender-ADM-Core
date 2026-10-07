#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Psapi requires the base Windows SDK types in this order.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <unistd.h>
#endif

#include "live_triple_balance_renderer.h"
#include "scene_output_session.h"
#include "scene_stream_engine.h"

namespace {
using namespace mradm;
using namespace mradm::live_scene;
using namespace mradm::realtime;
using Clock = std::chrono::steady_clock;
template <typename T> T take(Result<T> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
    return std::move(*value);
}
void done(Result<void> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
}
uint64_t rss() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        return info.resident_size;
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, static_cast<DWORD>(sizeof(counters))) != 0) {
        return counters.WorkingSetSize;
    }
#else
    std::ifstream status("/proc/self/statm");
    uint64_t virtual_pages = 0;
    uint64_t resident_pages = 0;
    const auto page_size = sysconf(_SC_PAGESIZE);
    if ((status >> virtual_pages >> resident_pages) && page_size > 0) {
        return resident_pages * static_cast<uint64_t>(page_size);
    }
#endif
    return 0;
}
struct Fixture {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes): fixture data shared by both benchmark paths.
    std::vector<ElementDescriptor> elements;
    std::vector<StateEntry> initial;
    Frame frame;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    Fixture() {
        constexpr std::array<const char*, 10> labels{
            "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
        frame.epoch_id = frame.generation_id = 1;
        frame.duration_samples = 480;
        frame.flags = frame_state_complete;
        for (uint64_t i = 0; i < 13; ++i) {
            ElementDescriptor descriptor;
            descriptor.element_id = i;
            descriptor.role = ElementRole::object;
            if (i < 10) {
                descriptor.role = i == 3 ? ElementRole::lfe : ElementRole::direct_speaker;
                descriptor.speaker_label = labels.at(i);
            }
            elements.push_back(descriptor);
            ObjectState state;
            state.valid_fields = k_known_state_fields & ~state_position;
            if (i >= 10) {
                state.valid_fields |= state_position;
                state.x = -.4F;
                state.y = .3F;
                state.z = .5F;
                state.width = state.height = state.depth = i < 12 ? .3F : 0;
            }
            initial.push_back({i, state});
            PcmPlane plane{i, true, std::vector<float>(480)};
            for (std::size_t f = 0; f < 480; ++f) {
                plane.samples[f] = static_cast<float>(std::sin(static_cast<double>(f * (i + 1)) * .03) * .01);
            }
            frame.pcm.push_back(std::move(plane));
        }
    }
    void update(uint64_t block) {
        frame.media_sample_start = static_cast<int64_t>(block * 480);
        frame.initial_states = block == 0 ? initial : std::vector<StateEntry>{};
        frame.updates.clear();
        for (uint64_t i : {10U, 11U}) {
            auto state = initial[i].state;
            state.x = block % 2 == 0 ? -.4F : .4F;
            state.width = state.height = state.depth = block % 2 == 0 ? .3F : .6F;
            frame.updates.push_back({i, 0, 480, false, state_position | state_extent, state});
        }
    }
};
} // namespace

int main(int argc, char** argv) {
#ifndef NDEBUG
    std::cerr << "Run this benchmark from a Release build.\n";
    return 2;
#endif
    try {
        const unsigned seconds = argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 60U;
        if (seconds < 10 || seconds > 600) {
            throw std::runtime_error("duration must be 10..600 seconds");
        }
        SceneStreamConfig config;
        config.renderer.renderer = RendererSelection::triple_balance;
        config.renderer.output_layout = argc > 1 ? argv[1] : "9.1.6";
        config.renderer.sample_rate = config.output_sample_rate = 48000;
        Fixture fixture;
        auto renderer = take(create_live_triple_balance_renderer(config.renderer, {}));
        done(renderer->configure_generation(1, fixture.elements));
        std::vector<float> pcm(std::size_t{480} * renderer->output_channels());
        const auto start = Clock::now();
        for (uint64_t block = 0; block < 1000; ++block) {
            fixture.update(block);
            done(renderer->render(fixture.frame, pcm));
        }
        const double rtf = std::chrono::duration<double>(Clock::now() - start).count() / 10.;
        renderer = nullptr;
        auto stream = std::shared_ptr<SceneStreamEngine>(take(SceneStreamEngine::create(config)));
        SceneDeviceConfig device;
        device.kind = SceneOutputKind::null_device;
        auto output = take(SceneOutputSession::create(stream, device));
        done(output->begin_epoch(1, 0));
        done(stream->configure_generation(1, 1, fixture.elements));
        std::vector<ScenePcmPlaneView> planes;
        planes.reserve(fixture.frame.pcm.size());
        std::ranges::transform(fixture.frame.pcm, std::back_inserter(planes), [](const auto& plane) {
            return ScenePcmPlaneView{plane.element_id, plane.samples.data(), 480, 1, true};
        });
        uint64_t early_rss = 0;
        uint64_t peak_rss = 0;
        const auto realtime_start = Clock::now();
        for (uint64_t block = 0; block < static_cast<uint64_t>(seconds) * 100; ++block) {
            fixture.update(block);
            SceneFrameView frame;
            frame.epoch_id = frame.generation_id = 1;
            frame.media_sample_start = fixture.frame.media_sample_start;
            frame.duration_samples = 480;
            frame.flags = frame_state_complete;
            frame.pcm = planes;
            frame.initial_states = fixture.frame.initial_states;
            frame.updates = fixture.frame.updates;
            const auto accepted = take(stream->submit_frame(frame, std::chrono::seconds(5)));
            if (accepted != SceneSubmitStatus::accepted) {
                throw std::runtime_error("benchmark producer timed out");
            }
            if (block == 15) {
                output->play();
            }
            if (block == 500) {
                early_rss = rss();
            }
            if (block % 100 == 0) {
                peak_rss = std::max(peak_rss, rss());
            }
            if (stream->status().failed) {
                throw std::runtime_error("benchmark Scene worker failed");
            }
        }
        done(stream->signal_end(1, static_cast<int64_t>(seconds) * 48000));
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!output->status().ended && !output->status().failed && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto status = output->status();
        const uint64_t final_rss = rss();
        std::cout << "layout=" << config.renderer.output_layout << " rtf=" << rtf
                  << " elapsed_s=" << std::chrono::duration<double>(Clock::now() - realtime_start).count()
                  << " underruns=" << status.underruns << " frames=" << status.presented_frames
                  << " rss_5s=" << early_rss << " rss_final=" << final_rss << " rss_peak=" << peak_rss << '\n';
        return rtf < 1 && status.ended && !status.failed && status.underruns == 0 &&
                       status.presented_frames == static_cast<uint64_t>(seconds) * 48000 &&
                       final_rss <= early_rss + (4ULL * 1024ULL * 1024ULL)
                   ? 0
                   : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
