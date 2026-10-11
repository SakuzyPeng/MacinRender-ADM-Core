#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
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
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <unistd.h>
#endif
#include <nlohmann/json.hpp>

#include "adm/room.h"

#include "live_room_renderer.h"
#include "room_ffi.h"
#include "room_worker_wait.h"
#include "scene_output_session.h"
#include "scene_stream_engine.h"

namespace {
using namespace mradm;
using namespace mradm::live_scene;
using namespace mradm::realtime;
using Clock = std::chrono::steady_clock;
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
double cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user)) {
        throw std::runtime_error("CPU clock");
    }
    const auto value = [](FILETIME v) { return (static_cast<uint64_t>(v.dwHighDateTime) << 32U) | v.dwLowDateTime; };
    return static_cast<double>(value(kernel) + value(user)) * 1e-7;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
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
    PROCESS_MEMORY_COUNTERS value{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &value, static_cast<DWORD>(sizeof(value))) != 0) {
        return value.WorkingSetSize;
    }
#else
    std::ifstream input("/proc/self/statm");
    uint64_t total = 0, resident = 0;
    input >> total >> resident;
    if (input) {
        return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    }
#endif
    return 0;
}
class Fixture {
  public:
    Fixture(std::size_t sources, uint32_t frames) {
        frame_.epoch_id = frame_.generation_id = 1;
        frame_.duration_samples = frames;
        for (std::size_t i = 0; i < sources; ++i) {
            ElementDescriptor element;
            element.element_id = i + 1;
            element.has_position = true;
            std::array<float, 3> position{};
            if (mradm_room_polar(static_cast<float>(i) * 360.F / static_cast<float>(sources),
                                 (static_cast<float>(i % 3) * 15.F) - 15.F,
                                 1.4F,
                                 position.data()) != 0) {
                throw std::runtime_error("position");
            }
            element.x = position[0];
            element.y = position[1];
            element.z = position[2];
            elements_.push_back(element);
            ObjectState state;
            state.valid_fields = k_known_state_fields;
            state.x = element.x;
            state.y = element.y;
            state.z = element.z;
            states_.push_back({element.element_id, state});
            frame_.pcm.push_back({element.element_id, true, std::vector<float>(frames)});
        }
    }
    void update(uint64_t block) {
        frame_.media_sample_start = static_cast<int64_t>(block * frame_.duration_samples);
        frame_.initial_states = block == 0 ? states_ : std::vector<StateEntry>{};
        for (std::size_t source = 0; source < frame_.pcm.size(); ++source) {
            auto& pcm = frame_.pcm[source].samples;
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                const auto word = ((block * pcm.size() + i) * 3943 + source * 7817) % 65521;
                pcm[i] = (static_cast<float>(word) - 32760.F) / 16777216.F;
            }
        }
    }
    [[nodiscard]] const std::vector<ElementDescriptor>& elements() const { return elements_; }
    [[nodiscard]] const Frame& frame() const { return frame_; }

  private:
    std::vector<ElementDescriptor> elements_;
    std::vector<StateEntry> states_;
    Frame frame_;
};
RendererConfig renderer_config(const RoomConfig& room) {
    RendererConfig config;
    config.renderer = RendererSelection::room_binaural;
    config.room = room;
    config.output_layout = "binaural";
    config.binaural_spread_mode = BinauralSpreadMode::none;
    return config;
}
void kernel(const RoomConfig& first, const RoomConfig& next, unsigned seconds, uint32_t quantum = 256) {
    set_room_worker_active(quantum == 512);
    for (const std::size_t sources : {1U, 16U, 32U}) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            auto renderer = take(create_live_room_renderer(renderer_config(first), {}));
            Fixture fixture(sources, quantum);
            done(renderer->configure_generation(1, fixture.elements()));
            std::vector<float> output(static_cast<std::size_t>(quantum) * 2);
            for (uint64_t block = 0; block < 64; ++block) {
                fixture.update(block);
                done(renderer->render(fixture.frame(), output));
            }
            const std::size_t blocks = static_cast<std::size_t>(seconds) * 48000 / quantum;
            std::vector<double> times(blocks);
            uint64_t peak_rss = rss();
            auto control = renderer->room_control();
            const double cpu_start = cpu_seconds();
            const auto begin = Clock::now();
            for (std::size_t block = 0; block < blocks; ++block) {
                fixture.update(block + 64);
                if (mode > 0) {
                    renderer->set_listener_orientation({(static_cast<float>(block % 720) * .25F) - 90.F, 10.F, 5.F});
                }
                if (mode == 2 && block == 96) {
                    take(control->request(next));
                }
                const auto tick = Clock::now();
                done(renderer->render(fixture.frame(), output));
                times[block] = std::chrono::duration<double>(Clock::now() - tick).count();
                if (block % 64 == 0) {
                    peak_rss = std::max(peak_rss, rss());
                }
            }
            const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
            const double cpu = cpu_seconds() - cpu_start;
            const double audio = static_cast<double>(blocks) * quantum / 48000.;
            const auto misses = std::ranges::count_if(times, [quantum](double t) { return t > quantum / 48000.; });
            std::ranges::sort(times);
            const auto status = control->status();
            std::cout << nlohmann::json{{"case", "kernel"},
                                        {"sources", sources},
                                        {"call_frames", quantum},
                                        {"pose_update_hz", mode > 0 ? 48000. / quantum : 0.},
                                        {"continuous_pose", mode > 0},
                                        {"room_switch", mode == 2},
                                        {"audio_seconds", audio},
                                        {"wall_seconds", elapsed},
                                        {"cpu_seconds", cpu},
                                        {"cpu_audio_budget_percent", 100. * cpu / audio},
                                        {"p99_ms", times.at(blocks * 99 / 100) * 1000.},
                                        {"maximum_ms", times.back() * 1000.},
                                        {"deadline_misses", misses},
                                        {"rss_peak_bytes", peak_rss},
                                        {"applied_revision", status.applied_revision},
                                        {"final_state", static_cast<unsigned>(status.state)}}
                             .dump()
                      << '\n'
                      << std::flush;
        }
    }
    set_room_worker_active(false);
}
void control_latency(const RoomConfig& room) {
    auto reference = take(create_live_room_renderer(renderer_config(room), {}));
    auto changed = take(create_live_room_renderer(renderer_config(room), {}));
    Fixture fixture(1, 256);
    done(reference->configure_generation(1, fixture.elements()));
    done(changed->configure_generation(1, fixture.elements()));
    std::array<float, 512> a{};
    std::array<float, 512> b{};
    for (uint64_t block = 0; block < 256; ++block) {
        fixture.update(block);
        done(reference->render(fixture.frame(), a));
        done(changed->render(fixture.frame(), b));
    }
    changed->set_listener_orientation({45.F, 15.F, 12.F});
    uint64_t first = UINT64_MAX;
    for (uint64_t block = 0; block < 8; ++block) {
        fixture.update(block + 256);
        done(reference->render(fixture.frame(), a));
        done(changed->render(fixture.frame(), b));
        for (std::size_t frame = 0; frame < 256; ++frame) {
            const auto left = frame * 2;
            const auto right = left + 1;
            if (first == UINT64_MAX && (a.at(left) != b.at(left) || a.at(right) != b.at(right))) {
                first = (block * 256) + frame;
            }
        }
    }
    if (first == UINT64_MAX) {
        throw std::runtime_error("head control produced no output change");
    }
    std::cout << nlohmann::json{{"case", "control_latency"},
                                {"frames_to_first_change", first},
                                {"milliseconds", static_cast<double>(first) / 48.},
                                {"scope", "Room PCM only; excludes host queue, sensor and physical device"}}
                     .dump()
              << '\n'
              << std::flush;
}
bool streaming(
    const RoomConfig& first, const RoomConfig& next, unsigned seconds, bool cycles = false, unsigned sources = 32) {
    SceneStreamConfig config;
    config.renderer = renderer_config(first);
    // Use the engine's existing default (capped at 2048 while head tracking).
    auto stream = std::shared_ptr<SceneStreamEngine>(take(SceneStreamEngine::create(config)));
    SceneDeviceConfig device;
    device.kind = SceneOutputKind::null_device;
    auto output = take(SceneOutputSession::create(stream, device));
    done(output->begin_epoch(1, 0));
    Fixture fixture(sources, 512);
    done(stream->configure_generation(1, 1, fixture.elements()));
    std::vector<ScenePcmPlaneView> planes;
    planes.reserve(fixture.frame().pcm.size());
    std::ranges::transform(fixture.frame().pcm, std::back_inserter(planes), [](const auto& pcm) {
        return ScenePcmPlaneView{pcm.element_id, pcm.samples.data(), 512, 1, true};
    });
    auto control = stream->room_control();
    const auto begin = Clock::now();
    const double cpu_start = cpu_seconds();
    std::jthread tracking([&](const std::stop_token& stop) {
        uint64_t requested = 0;
        uint64_t tick = 0;
        while (!stop.stop_requested()) {
            const double time = std::chrono::duration<double>(Clock::now() - begin).count();
            stream->set_listener_orientation({(static_cast<float>(tick % 1440) * .25F) - 180.F,
                                              static_cast<float>(std::sin(time * .7) * 20.),
                                              static_cast<float>(std::sin(time * .4) * 10.)});
            if ((cycles || requested == 0) && time >= 2. + (static_cast<double>(requested) * 5.)) {
                auto accepted = control->request(requested % 2 == 0 ? next : first);
                if (accepted) {
                    ++requested;
                }
            }
            ++tick;
            std::this_thread::sleep_until(begin + std::chrono::milliseconds(tick * 10));
        }
    });
    uint64_t peak_rss = rss();
    uint64_t last_underruns = 0;
    nlohmann::json underrun_events = nlohmann::json::array();
    const uint64_t frames = static_cast<uint64_t>(seconds) * 48000;
    for (uint64_t sample = 0, block = 0; sample < frames; ++block) {
        fixture.update(block);
        SceneFrameView frame;
        frame.epoch_id = frame.generation_id = 1;
        frame.media_sample_start = static_cast<int64_t>(sample);
        frame.duration_samples = static_cast<uint32_t>(std::min<uint64_t>(512, frames - sample));
        for (auto& plane : planes) {
            plane.sample_count = frame.duration_samples;
        }
        frame.flags = frame_state_complete;
        frame.pcm = planes;
        frame.initial_states = fixture.frame().initial_states;
        if (take(stream->submit_frame(frame, std::chrono::seconds(5))) != SceneSubmitStatus::accepted) {
            throw std::runtime_error("producer timeout");
        }
        sample += frame.duration_samples;
        if (block == 4) {
            output->play();
        }
        if (block % 32 == 0) {
            peak_rss = std::max(peak_rss, rss());
        }
        const auto current = output->status();
        if (current.underruns != last_underruns) {
            last_underruns = current.underruns;
            underrun_events.push_back({{"seconds", std::chrono::duration<double>(Clock::now() - begin).count()},
                                       {"count", last_underruns},
                                       {"room_state", static_cast<unsigned>(control->status().state)}});
        }
    }
    done(stream->signal_end(1, static_cast<int64_t>(frames)));
    const auto limit = Clock::now() + std::chrono::seconds(8);
    while (!output->status().ended && !output->status().failed && Clock::now() < limit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    tracking.request_stop();
    tracking.join();
    const auto status = output->status();
    const auto room = control->status();
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    std::cout << nlohmann::json{{"case", "Scene_streaming_null_device"},
                                {"sources", sources},
                                {"head_update_hz", 100},
                                {"room_switch", true},
                                {"repeated_room_switches", cycles},
                                {"cpu_percent", 100. * (cpu_seconds() - cpu_start) / elapsed},
                                {"wall_seconds", elapsed},
                                {"rss_peak_bytes", peak_rss},
                                {"presented_frames", status.presented_frames},
                                {"expected_frames", frames + room.tail_frames},
                                {"underruns", status.underruns},
                                {"underrun_events", underrun_events},
                                {"ended", status.ended},
                                {"failed", status.failed},
                                {"applied_revision", room.applied_revision}}
                     .dump()
              << '\n'
              << std::flush;
    return status.ended && !status.failed && status.underruns == 0 &&
           status.presented_frames == frames + room.tail_frames && room.applied_revision != 0;
}
} // namespace
int main(int argc, char** argv) {
#ifndef NDEBUG
    std::cerr << "Use a Release build.\n";
    return 2;
#endif
    try {
        if (argc < 3) {
            throw std::runtime_error("usage: room_scene_benchmark ROOM_JSON OTHER_ROOM_JSON [SECONDS=10] "
                                     "[all|stream|stream-cycles|kernel|kernel-worker|latency] [STREAM_SOURCES=32]");
        }
        const unsigned seconds = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 10;
        if (seconds < 5 || seconds > 120) {
            throw std::runtime_error("seconds must be 5..120");
        }
        RoomConfig first{take(RoomProfile::load(argv[1])), {}, 1.5};
        RoomConfig next{take(RoomProfile::load(argv[2])), {}, 1.5};
        const std::string_view mode = argc > 4 ? argv[4] : "all";
        const unsigned sources = argc > 5 ? static_cast<unsigned>(std::stoul(argv[5])) : 32;
        if (sources != 1 && sources != 16 && sources != 32) {
            throw std::runtime_error("stream sources must be 1, 16 or 32");
        }
        if (mode == "all" || mode == "kernel" || mode == "kernel-worker") {
            kernel(first, next, seconds, mode == "kernel-worker" ? 512 : 256);
        }
        if (mode == "all" || mode == "latency") {
            control_latency(first);
        }
        if (mode == "all" || mode == "stream" || mode == "stream-cycles") {
            return streaming(first, next, seconds, mode == "stream-cycles", sources) ? 0 : 1;
        }
        if (mode != "kernel" && mode != "kernel-worker" && mode != "latency") {
            throw std::runtime_error("unknown benchmark mode");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
