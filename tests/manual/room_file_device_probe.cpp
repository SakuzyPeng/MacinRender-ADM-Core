#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif

#include <nlohmann/json.hpp>

#include "adm/io.h"
#include "adm/monitor.h"
#include "adm/room.h"

#include "room_worker_wait.h"

namespace {
using namespace mradm;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
template <class T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}
uint64_t resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, static_cast<DWORD>(sizeof(memory))) != 0) {
        return memory.WorkingSetSize;
    }
#endif
    return 0;
}
} // namespace

// NOLINTNEXTLINE(readability-function-size): one manual playback lifecycle and its evidence.
int main(int argc, char** argv) {
#ifndef NDEBUG
    std::cerr << "Use a Release build.\n";
    return 2;
#endif
    try {
        if (argc != 5) {
            throw std::runtime_error("usage: room_file_device_probe INPUT_ADM ROOM_JSON OTHER_ROOM_JSON DEVICE_ID");
        }
        const auto devices = MonitorSession::list_output_devices();
        const auto device = std::ranges::find(devices, std::string(argv[4]), &AudioOutputDeviceInfo::id);
        if (device == devices.end()) {
            throw std::runtime_error("Select an exact current device ID; no default fallback is allowed");
        }
        const auto scene = take(io::import_scene(argv[1]));
        RenderOptions options;
        options.renderer = RendererSelection::room_binaural;
        options.output_layout = "binaural";
        options.binaural_spread_mode = BinauralSpreadMode::none;
        options.room = {take(RoomProfile::load(argv[2])), {}, 1.5, RoomQuality::realtime};
        const RoomConfig next{take(RoomProfile::load(argv[3])), {}, 1.5, RoomQuality::realtime};
        auto monitor = take(MonitorSession::create(argv[1], options, device->id));
        const auto control = monitor->room_control();
        if (!control || control->status().quality != RoomQuality::realtime) {
            throw std::runtime_error("File monitor did not select the realtime room backend");
        }
        const auto rate = scene.info.sample_rate;
        const auto frames = scene.info.num_frames;
        const double requested_seconds = static_cast<double>(frames) / rate;
        const auto origin = Clock::now();
        Json memory = Json::array();
        Json underruns = Json::array();
        uint64_t last_underruns = 0;
        uint64_t peak_rss = resident_bytes();
        uint64_t poses = 0;
        uint64_t switches = 0;
        uint64_t next_second = 0;
        double max_pose_lateness_ms = 0;
        float peak = 0;
        uint64_t max_buffered = 0;
        std::cout << Json{{"case", "file_probe_ready"},
                          {"device", device->name},
                          {"quality", "realtime"},
                          {"channels", scene.info.num_channels},
                          {"objects", scene.objects.size()},
                          {"requested_seconds", requested_seconds},
                          {"sample_rate", rate}}
                         .dump()
                  << '\n'
                  << std::flush;
        monitor->set_listener_orientation({0, 0, 0});
        monitor->play();
        while (true) {
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - origin).count();
            const auto status = monitor->status();
            // ended marks producer EOS; the real device still owns buffered tail samples.
            if ((status.ended && status.buffered_frames == 0) || status.failed || elapsed > requested_seconds + 30.) {
                break;
            }
            const double pose_time = static_cast<double>(poses) / 100.;
            max_pose_lateness_ms = std::max(max_pose_lateness_ms, (elapsed - pose_time) * 1000.);
            monitor->set_listener_orientation({static_cast<float>(std::sin(pose_time * .25) * 90.),
                                               static_cast<float>(std::sin(pose_time * .7) * 20.),
                                               static_cast<float>(std::sin(pose_time * .4) * 10.)});
            ++poses;
            if (status.playhead_frames + (static_cast<uint64_t>(rate) * 3) < frames &&
                elapsed >= 2. + (static_cast<double>(switches) * 5.)) {
                take(control->request(switches % 2 == 0 ? next : options.room));
                ++switches;
            }
            if (control->status().state == RoomSwitchState::failed) {
                throw std::runtime_error(control->status().message);
            }
            if (status.underruns != last_underruns) {
                last_underruns = status.underruns;
                underruns.push_back({{"seconds", elapsed},
                                     {"count", last_underruns},
                                     {"room_state", static_cast<unsigned>(control->status().state)}});
            }
            max_buffered = std::max(max_buffered, status.buffered_frames);
            if (elapsed >= static_cast<double>(next_second)) {
                const auto rss = resident_bytes();
                peak_rss = std::max(peak_rss, rss);
                memory.push_back({{"seconds", elapsed}, {"rss_bytes", rss}});
                const auto levels = monitor->levels();
                for (uint32_t i = 0; i < levels.channels; ++i) {
                    peak = std::max(peak, levels.peak.at(i));
                }
                std::cout << Json{{"case", "file_device_progress"},
                                  {"seconds", elapsed},
                                  {"playhead_frames", status.playhead_frames},
                                  {"underruns", status.underruns},
                                  {"buffered_frames", status.buffered_frames},
                                  {"rss_bytes", rss},
                                  {"applied_revision", control->status().applied_revision}}
                                 .dump()
                          << '\n'
                          << std::flush;
                ++next_second;
            }
            const auto deadline = origin + std::chrono::milliseconds(poses * 10);
#ifdef _WIN32
            while (Clock::now() < deadline) {
                realtime::wait_room_worker(std::chrono::milliseconds(1));
            }
#else
            std::this_thread::sleep_until(deadline);
#endif
        }
        const auto status = monitor->status();
        const auto room = control->status();
        Json logs = Json::array();
        for (std::size_t i = 0; i < monitor->log_count(); ++i) {
            LogLevel level{};
            std::string module;
            std::string message;
            if (monitor->log_entry(i, level, module, message)) {
                logs.push_back({{"level", static_cast<unsigned>(level)}, {"module", module}, {"message", message}});
            }
        }
        const double wall = std::chrono::duration<double>(Clock::now() - origin).count();
        const bool passed = status.ended && !status.failed && status.underruns == 0 &&
                            status.playhead_frames == frames + room.tail_frames && peak > 0.F &&
                            room.applied_revision == switches && static_cast<double>(poses) >= requested_seconds * 99.;
        monitor.reset();
        std::cout << Json{{"case", "file_streaming_device"},
                          {"passed", passed},
                          {"device", device->name},
                          {"quality", "realtime"},
                          {"requested_seconds", requested_seconds},
                          {"sample_rate", rate},
                          {"input_channels", scene.info.num_channels},
                          {"objects", scene.objects.size()},
                          {"wall_seconds", wall},
                          {"presented_frames", status.playhead_frames},
                          {"expected_frames", frames + room.tail_frames},
                          {"underruns", status.underruns},
                          {"underrun_events", underruns},
                          {"ended", status.ended},
                          {"failed", status.failed},
                          {"poses_sent", poses},
                          {"pose_sender_max_lateness_ms", max_pose_lateness_ms},
                          {"room_switch_requests", switches},
                          {"applied_revision", room.applied_revision},
                          {"sampled_peak", peak},
                          {"maximum_host_buffered_frames", max_buffered},
                          {"rss_peak_bytes", peak_rss},
                          {"memory_samples", memory},
                          {"logs", logs},
                          {"scope",
                           "MonitorSession ADM import, file streaming PCM decode, rendering and real audio device; "
                           "simulated 100 Hz head input"}}
                         .dump()
                  << '\n'
                  << std::flush;
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << Json{{"case", "file_probe_error"}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
