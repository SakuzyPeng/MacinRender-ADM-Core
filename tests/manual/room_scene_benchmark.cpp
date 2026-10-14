#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
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

#include "adm/head_tracking.h"
#include "adm/room.h"

#include "live_room_renderer.h"
#include "room_ffi.h"
#include "room_probe_metrics.h"
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
    Fixture(std::size_t sources, uint32_t frames, bool broadband = false, bool moving = false)
        : broadband_(broadband), moving_(moving) {
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
        frame_.updates.clear();
        for (std::size_t source = 0; source < frame_.pcm.size(); ++source) {
            if (moving_) {
                MetadataUpdate movement;
                movement.element_id = source + 1;
                movement.offset_samples = static_cast<uint32_t>((source * 131 + 17) % frame_.duration_samples);
                movement.ramp_duration_samples = frame_.duration_samples;
                movement.changed_fields = movement.state.valid_fields = state_position;
                std::array<float, 3> position{};
                if (mradm_room_polar((static_cast<float>(source) * 360.F / static_cast<float>(frame_.pcm.size())) +
                                         (static_cast<float>((block + 1) % 720) * .5F),
                                     (static_cast<float>(source % 3) * 15.F) - 15.F,
                                     1.4F,
                                     position.data()) != 0) {
                    throw std::runtime_error("moving position");
                }
                movement.state.x = position[0];
                movement.state.y = position[1];
                movement.state.z = position[2];
                frame_.updates.push_back(movement);
            }
            auto& pcm = frame_.pcm[source].samples;
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                if (broadband_) {
                    // Fixed PCG hash noise avoids ambiguous periodic correlation
                    // peaks in device loopback measurements. No mutable RNG state.
                    const auto index = static_cast<std::uint32_t>((block * pcm.size()) + i);
                    const auto salt = static_cast<std::uint32_t>(source) * 3812015801U;
                    const std::uint32_t state = ((index * 747796405U) + 2891336453U) ^ salt;
                    const auto word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
                    const auto bits = (word >> 22U) ^ word;
                    pcm[i] = (static_cast<float>(bits >> 8U) - 8388608.F) / 4294967296.F;
                    continue;
                }
                const auto word = ((block * pcm.size() + i) * 3943 + source * 7817) % 65521;
                pcm[i] = (static_cast<float>(word) - 32760.F) / 16777216.F;
            }
        }
        std::ranges::stable_sort(frame_.updates, {}, &MetadataUpdate::offset_samples);
    }
    [[nodiscard]] const std::vector<ElementDescriptor>& elements() const { return elements_; }
    [[nodiscard]] const Frame& frame() const { return frame_; }

  private:
    std::vector<ElementDescriptor> elements_;
    std::vector<StateEntry> states_;
    Frame frame_;
    bool broadband_;
    bool moving_;
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
struct ProbeOptions {
    std::string device_name;
    std::string capture_name;
    std::filesystem::path recordings;
    bool loopback{false};
    bool moving{false};
    int osc_port{-1};
    float volume{.1F};
};
AudioDeviceInfo select_device(const std::string& selection) {
    std::vector<AudioDeviceInfo> matches;
    std::ranges::copy_if(enumerate_output_devices(), std::back_inserter(matches), [&](const auto& info) {
        return info.id == selection || info.name == selection;
    });
    if (matches.size() != 1) {
        throw std::runtime_error("Output selection must match exactly one current device name or ID");
    }
    return matches.front();
}
// Keep preparation, bounded observation, and joined teardown together in this manual probe.
// NOLINTNEXTLINE(readability-function-size)
bool streaming(const RoomConfig& first,
               const RoomConfig& next,
               unsigned seconds,
               bool cycles,
               unsigned sources,
               const ProbeOptions& options) {
    SceneStreamConfig config;
    config.renderer = renderer_config(first);
    auto stream = std::shared_ptr<SceneStreamEngine>(take(SceneStreamEngine::create(config)));
    const bool real_device = !options.device_name.empty();
    const auto selected = real_device ? select_device(options.device_name) : AudioDeviceInfo{};
    const bool capture = options.loopback || !options.capture_name.empty();
    if (capture && (!real_device || options.recordings.empty())) {
        throw std::runtime_error("Capture requires --device and a new --recordings directory");
    }
    if (!options.recordings.empty() && !std::filesystem::create_directories(options.recordings)) {
        throw std::runtime_error("Recordings directory must not already exist");
    }
    std::unique_ptr<OscHeadTrackingReceiver> receiver;
    if (options.osc_port >= 0) {
        receiver =
            std::make_unique<OscHeadTrackingReceiver>(static_cast<std::uint16_t>(options.osc_port), "room-probe");
        done(receiver->start());
    }
    const auto begin = Clock::now();
    auto trace = std::make_shared<room_probe::OutputTrace>(begin, seconds, capture);
    std::unique_ptr<room_probe::CaptureTap> tap;
    if (capture) {
        tap = std::make_unique<room_probe::CaptureTap>(
            begin, seconds, options.capture_name, options.loopback ? selected.id : "");
    }
    auto device = std::make_unique<room_probe::MeasuredDevice>(make_miniaudio_device(!real_device, selected.id), trace);
    auto output = take(SceneOutputSession::create_with_device(stream, std::move(device), true));
    done(output->set_volume(options.volume));
    done(output->begin_epoch(1, 0));
    Fixture fixture(sources, 512, true, options.moving);
    done(stream->configure_generation(1, 1, fixture.elements()));
    std::vector<ScenePcmPlaneView> planes;
    planes.reserve(fixture.frame().pcm.size());
    std::ranges::transform(fixture.frame().pcm, std::back_inserter(planes), [](const auto& pcm) {
        return ScenePcmPlaneView{pcm.element_id, pcm.samples.data(), 512, 1, true};
    });
    auto control = stream->room_control();
    std::cout << nlohmann::json{{"case", "probe_ready"},
                                {"device", real_device ? selected.name : "null"},
                                {"osc_port", receiver ? receiver->snapshot().bound_port : 0},
                                {"seconds", seconds},
                                {"sample_rate", 48000},
                                {"quality", first.quality == RoomQuality::realtime ? "realtime" : "exact"},
                                {"source_ramps", options.moving},
                                {"volume", options.volume}}
                     .dump()
              << '\n'
              << std::flush;
    const double cpu_start = cpu_seconds();
    std::atomic<bool> tracking_failed{false};
    std::atomic<bool> material_ended{false};
    std::string tracking_error;
    std::vector<double> control_age_ms;
    std::vector<double> control_call_ms;
    const auto prepare_samples = [](std::vector<double>& values, std::size_t count) {
        values.resize(count);
        for (std::size_t i = 0; i < count; i += 512U) {
            volatile double& value = values[i];
            value = 0.;
        }
        values.clear();
    };
    prepare_samples(control_age_ms, static_cast<std::size_t>(seconds + 10) * 110U);
    prepare_samples(control_call_ms, control_age_ms.capacity());
    std::uint64_t requested = 0;
    std::uint64_t poses_applied = 0;
    std::jthread tracking([&](const std::stop_token& stop) {
        try {
            const auto tracking_origin = Clock::now();
            std::uint64_t tick = 0;
            std::uint64_t sequence = 0;
            const auto interval = receiver ? std::chrono::milliseconds(1) : std::chrono::milliseconds(10);
            while (!stop.stop_requested()) {
                const double time = std::chrono::duration<double>(Clock::now() - begin).count();
                bool apply = true;
                ListenerOrientation orientation{static_cast<float>(std::sin(time * .25) * 90.),
                                                static_cast<float>(std::sin(time * .7) * 20.),
                                                static_cast<float>(std::sin(time * .4) * 10.)};
                if (receiver) {
                    const auto pose = receiver->snapshot();
                    if (pose.state == OscHeadTrackingState::failed) {
                        throw std::runtime_error(receiver->last_error());
                    }
                    apply = pose.fresh && pose.sequence != sequence;
                    if (apply) {
                        sequence = pose.sequence;
                        orientation = {pose.orientation.euler_deg[0],
                                       pose.orientation.euler_deg[1],
                                       pose.orientation.euler_deg[2]};
                        control_age_ms.push_back(static_cast<double>(pose.age_ms));
                    }
                }
                if (apply) {
                    const auto start = Clock::now();
                    stream->set_listener_orientation(orientation);
                    control_call_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    ++poses_applied;
                }
                if (!material_ended.load() && (cycles || requested == 0) &&
                    time >= 2. + (static_cast<double>(requested) * 5.)) {
                    const auto accepted = control->request(requested % 2 == 0 ? next : first);
                    if (accepted) {
                        ++requested;
                    } else if (!material_ended.load()) {
                        throw std::runtime_error(accepted.error().message);
                    }
                }
                if (control->status().state == RoomSwitchState::failed) {
                    throw std::runtime_error(control->status().message);
                }
                ++tick;
#ifdef _WIN32
                // Sleep(1) may sleep for a full Windows timer tick. Poll the
                // latest OSC snapshot with the existing thread-owned timer.
                const auto deadline = tracking_origin + interval * static_cast<std::int64_t>(tick);
                while (Clock::now() < deadline && !stop.stop_requested()) {
                    wait_room_worker(std::chrono::milliseconds(1));
                }
#else
                std::this_thread::sleep_until(tracking_origin + interval * static_cast<std::int64_t>(tick));
#endif
            }
        } catch (const std::exception& error) {
            tracking_error = error.what();
            tracking_failed.store(true);
        }
    });
    uint64_t peak_rss = rss();
    uint64_t last_underruns = 0;
    nlohmann::json underrun_events = nlohmann::json::array();
    struct MemoryPoint {
        double seconds;
        std::uint64_t bytes;
    };
    std::vector<MemoryPoint> resident_samples(static_cast<std::size_t>(seconds + 10));
    for (std::size_t i = 0; i < resident_samples.size(); i += 128U) {
        volatile std::uint64_t& value = resident_samples[i].bytes;
        value = 0;
    }
    resident_samples.clear();
    std::vector<double> queued_ms;
    prepare_samples(queued_ms, static_cast<std::size_t>(seconds + 10) * 2U);
    auto next_observation = Clock::now();
    const uint64_t frames = static_cast<uint64_t>(seconds) * 48000;
    for (uint64_t sample = 0, block = 0; sample < frames; ++block) {
        if (tracking_failed.load()) {
            break;
        }
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
        frame.updates = fixture.frame().updates;
        // The last material slice can be shorter than a canonical producer frame.
        while (!frame.updates.empty() && frame.updates.back().offset_samples >= frame.duration_samples) {
            frame.updates = frame.updates.first(frame.updates.size() - 1);
        }
        if (take(stream->submit_frame(frame, std::chrono::seconds(5))) != SceneSubmitStatus::accepted) {
            throw std::runtime_error("producer timeout");
        }
        sample += frame.duration_samples;
        if (block == 4) {
            output->play();
        }
        const auto current = output->status();
        if (current.failed || current.ended) {
            throw std::runtime_error("Unexpected device/Scene failure or premature EOS");
        }
        if (current.underruns != last_underruns) {
            last_underruns = current.underruns;
            underrun_events.push_back({{"seconds", std::chrono::duration<double>(Clock::now() - begin).count()},
                                       {"count", last_underruns},
                                       {"room_state", static_cast<unsigned>(control->status().state)}});
        }
        if (Clock::now() >= next_observation) {
            const double time = std::chrono::duration<double>(Clock::now() - begin).count();
            const auto resident = rss();
            peak_rss = std::max(peak_rss, resident);
            resident_samples.push_back({time, resident});
            queued_ms.push_back(static_cast<double>(current.queued_frames) / 48.);
            std::cout << nlohmann::json{{"case", "device_progress"},
                                        {"seconds", time},
                                        {"rss_bytes", resident},
                                        {"presented_frames", current.presented_frames},
                                        {"underruns", current.underruns}}
                             .dump()
                      << '\n'
                      << std::flush;
            next_observation += std::chrono::seconds(1);
        }
    }
    // Continue tracking throughout the tail, but do not request a new room
    // after EOS. Device setup time must not create a spurious final request.
    material_ended.store(true);
    done(stream->signal_end(1, static_cast<int64_t>(frames)));
    const auto limit = Clock::now() + std::chrono::seconds(8);
    while (!output->status().ended && !output->status().failed && Clock::now() < limit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    tracking.request_stop();
    tracking.join();
    const auto status = output->status();
    const auto room = control->status();
    const auto osc = receiver ? nlohmann::json::parse(receiver->snapshot_json()) : nlohmann::json(nullptr);
    output.reset(); // Stop both callbacks before inspecting their owned trace data.
    if (tap) {
        tap->stop();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    auto memory_samples = nlohmann::json::array();
    std::ranges::transform(resident_samples, std::back_inserter(memory_samples), [](const auto& point) {
        return nlohmann::json{{"seconds", point.seconds}, {"rss_bytes", point.bytes}};
    });
    nlohmann::json report{{"case", real_device ? "Scene_streaming_device" : "Scene_streaming_null_device"},
                          {"device", real_device ? selected.name : "null"},
                          {"sources", sources},
                          {"quality", first.quality == RoomQuality::realtime ? "realtime" : "exact"},
                          {"source_ramps", options.moving},
                          {"requested_seconds", seconds},
                          {"control_transport", receiver ? "OSC loopback UDP" : "direct API"},
                          {"nominal_head_update_hz", 100},
                          {"poses_applied", poses_applied},
                          {"room_switch_requests", requested},
                          {"repeated_room_switches", cycles},
                          {"cpu_percent", 100. * (cpu_seconds() - cpu_start) / elapsed},
                          {"wall_seconds", elapsed},
                          {"rss_peak_bytes", peak_rss},
                          {"memory_samples", memory_samples},
                          {"presented_frames", status.presented_frames},
                          {"consumed_frames", status.consumed_frames},
                          {"expected_frames", frames + room.tail_frames},
                          {"underruns", status.underruns},
                          {"underrun_events", underrun_events},
                          {"ended", status.ended},
                          {"failed", status.failed || tracking_failed.load()},
                          {"control_error", tracking_error},
                          {"applied_revision", room.applied_revision},
                          {"osc", osc},
                          {"volume", options.volume},
                          {"callback_interval", trace->intervals.report()},
                          {"callback_pull_duration", trace->pull_duration.report()},
                          {"control_call_duration_ms", room_probe::distribution(control_call_ms)},
                          {"osc_receive_to_apply_ms", room_probe::distribution(control_age_ms)},
                          {"osc_age_quantization_ms", 1},
                          {"host_queued_audio_ms", room_probe::distribution(queued_ms)},
                          {"clock_scope", "presented_frames is callback consumption, not a physical DAC clock"},
                          {"latency_scope",
                           "OSC receiver snapshot age to API apply and host queue depth; excludes sensor, kernel "
                           "response, driver and headphone"}};
    if (tap) {
        report["output_recording"] = trace->recording.save(options.recordings, "output");
        report["input_recording"] = tap->save(options.recordings);
        report["recording_scope"] =
            "output callback PCM to selected input callback; excludes sensor and pre-callback processing";
        std::ofstream file(options.recordings / "recordings.json");
        file << report.dump(2) << '\n';
        if (!file) {
            throw std::runtime_error("Cannot save capture metadata");
        }
    }
    std::cout << report.dump() << '\n' << std::flush;
    return status.ended && !status.failed && !tracking_failed.load() && status.underruns == 0 &&
           status.presented_frames == frames + room.tail_frames && room.applied_revision != 0 && poses_applied != 0;
}

} // namespace
// NOLINTNEXTLINE(readability-function-size): manual probe option parsing and mode dispatch.
int main(int argc, char** argv) {
#ifndef NDEBUG
    std::cerr << "Use a Release build.\n";
    return 2;
#endif
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--list-devices") {
            nlohmann::json devices = nlohmann::json::array();
            std::ranges::transform(enumerate_output_devices(), std::back_inserter(devices), [](const auto& info) {
                return nlohmann::json{{"id", info.id}, {"name", info.name}, {"default", info.is_default}};
            });
            std::cout << nlohmann::json{{"outputs", devices}, {"inputs", room_probe::CaptureTap::inputs()}}.dump(2)
                      << '\n';
            return 0;
        }
        if (argc < 3) {
            throw std::runtime_error(
                "usage: room_scene_benchmark ROOM_JSON OTHER_ROOM_JSON [SECONDS=10] "
                "[all|stream|stream-cycles|stream-device|kernel|kernel-worker|latency] [SOURCES=32] [--device NAME|ID] "
                "[--osc-port PORT] [--capture NAME|--capture-loopback] [--recordings NEW_DIR] [--volume GAIN] "
                "[--quality exact|realtime] [--source-ramps]");
        }
        const unsigned seconds = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 10;
        if (seconds < 5 || seconds > 3600) {
            throw std::runtime_error("seconds must be 5..3600");
        }
        RoomConfig first{take(RoomProfile::load(argv[1])), {}, 1.5};
        RoomConfig next{take(RoomProfile::load(argv[2])), {}, 1.5};
        const std::string_view mode = argc > 4 ? argv[4] : "all";
        const unsigned sources = argc > 5 ? static_cast<unsigned>(std::stoul(argv[5])) : 32;
        if (sources != 1 && sources != 16 && sources != 32) {
            throw std::runtime_error("stream sources must be 1, 16 or 32");
        }
        ProbeOptions options;
        for (int i = 6; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--capture-loopback") {
                options.loopback = true;
                continue;
            }
            if (flag == "--source-ramps") {
                options.moving = true;
                continue;
            }
            if (++i >= argc) {
                throw std::runtime_error("Missing probe option value");
            }
            if (flag == "--device") {
                options.device_name = argv[i];
            } else if (flag == "--capture") {
                options.capture_name = argv[i];
            } else if (flag == "--recordings") {
                options.recordings = argv[i];
            } else if (flag == "--volume") {
                options.volume = std::stof(argv[i]);
            } else if (flag == "--osc-port") {
                options.osc_port = std::stoi(argv[i]);
            } else if (flag == "--quality") {
                const std::string_view quality = argv[i];
                if (quality != "exact" && quality != "realtime") {
                    throw std::runtime_error("quality must be exact or realtime");
                }
                first.quality = next.quality = quality == "realtime" ? RoomQuality::realtime : RoomQuality::exact;
            } else {
                throw std::runtime_error("Unknown probe option");
            }
        }
        if (!std::isfinite(options.volume) || options.volume <= 0.F || options.volume > 1.F || options.osc_port < -1 ||
            options.osc_port > 65535 || (options.loopback && !options.capture_name.empty()) ||
            (mode == "stream-device" && options.device_name.empty())) {
            throw std::runtime_error("Invalid device probe options");
        }
        if (mode == "all" || mode == "kernel" || mode == "kernel-worker") {
            kernel(first, next, seconds, mode == "kernel-worker" ? 512 : 256);
        }
        if (mode == "all" || mode == "latency") {
            control_latency(first);
        }
        if (mode == "all" || mode == "stream" || mode == "stream-cycles" || mode == "stream-device") {
            return streaming(first, next, seconds, mode == "stream-cycles" || mode == "stream-device", sources, options)
                       ? 0
                       : 1;
        }
        if (mode != "kernel" && mode != "kernel-worker" && mode != "latency") {
            throw std::runtime_error("unknown benchmark mode");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << nlohmann::json{{"case", "probe_error"}, {"failed", true}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
