// Sample-clock replay of the real Scene worker and device-bound output DSP.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>

#include "adm/render_binaural.h"

#include "phase2_io.h"
#include "scene_output_session.h"
#include "scene_stream_engine.h"

namespace {
using namespace mradm;
using namespace mradm::realtime;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using phase2::require;
constexpr uint32_t k_signal_seed = 0x12345678U;
constexpr uint32_t k_main_epoch_end = 12289U;
constexpr uint32_t k_seek_target = 257U;
constexpr uint32_t k_seek_end = 290U;

float signal_scale(bool device_dsp) {
    return device_dsp ? 4.0F : 0.125F;
}

void validate_spec(const Json& spec) {
    // Version 1 describes a fixed control script. Reject divergent descriptions
    // before writing evidence; metadata, partition and renderer parameters are
    // the fields this runner actually reads while replaying.
    require(spec.contains("device_dsp") && spec.at("device_dsp").is_boolean(), "invalid replay device DSP flag");
    const bool device_dsp = spec.at("device_dsp");
    const bool triple = spec.at("backend") == "triple-balance";
    const bool sofa = spec.contains("sofa");
    require(!sofa || (spec.at("backend") == "binaural" && !device_dsp), "SOFA replay requires binaural rendering");
    int version = triple ? 2 : 1;
    std::string switch_control = triple ? "switch speaker backend" : "switch stereo backend";
    if (sofa) {
        version = 3;
        switch_control = "switch SOFA to built-in KEMAR";
    }
    Json fixed{{"version", version},
               {"clock", "epoch*2s + input_sample/input_rate; +1s at sample 10240"},
               {"state_complete_on_every_frame", true},
               {"signal", {{"generator", "lcg32-v1"}, {"seed", k_signal_seed}, {"scale", signal_scale(device_dsp)}}},
               {"epochs",
                {{{"epoch", 1}, {"target", 0}, {"end", k_main_epoch_end}},
                 {{"epoch", 2}, {"target", k_seek_target}, {"end", k_seek_end}}}},
               {"controls",
                {{"2048", "pose(37,23,-19)"},
                 {"4096", "generation=2"},
                 {"6144", "semantic gain scale=0.5"},
                 {"8192", switch_control},
                 {"10240", "advance virtual clock by 1s; expire tracking"}}}};
    if (device_dsp) {
        fixed["output_controls"] = {{"0", "volume=0.875; HpTF bypass"},
                                    {"2048", "HpTF peak 1700Hz +6dB Q0.75 preamp -3dB auto-trim"},
                                    {"4096", "HpTF high-shelf 4200Hz -4dB Q0.75 preamp -6dB"},
                                    {"6144", "HpTF bypass"},
                                    {"8192", "volume=0.5"}};
    } else {
        require(!spec.contains("output_controls"), "replay output controls require device DSP");
    }
    for (const auto& [field, expected] : fixed.items()) {
        require(spec.contains(field), "missing fixed replay field");
        if (spec.at(field) != expected) {
            throw std::runtime_error("unsupported replay field: " + field);
        }
    }
    require(triple || spec.at("backend") == "vbap" || spec.at("backend") == "binaural", "unknown replay backend");
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
void save_json(const std::filesystem::path& path, const Json& data) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << data.dump(2) << '\n';
    out.close();
}
class Capture final : public IAudioOutputDevice {
  public:
    Result<void> start(uint32_t channels, uint32_t rate, PullFn fn) override {
        channels_ = channels;
        rate_ = rate;
        pull_ = std::move(fn);
        return {};
    }
    void stop() override { pull_ = {}; }
    [[nodiscard]] uint32_t actual_sample_rate() const override { return rate_; }
    std::size_t pull(std::span<float> data) { return pull_(data, data.size() / channels_); }

  private:
    PullFn pull_;
    uint32_t channels_{0}, rate_{0};
};
class Replay {
  public:
    Replay(const Json& case_spec, const std::filesystem::path& sofa_path)
        : spec(case_spec), device_dsp(case_spec.at("device_dsp").get<bool>()) {
        config.renderer.renderer =
            spec.at("backend") == "vbap" ? RendererSelection::saf : RendererSelection::saf_binaural;
        if (spec.at("backend") == "triple-balance") {
            config.renderer.renderer = RendererSelection::triple_balance;
        }
        config.renderer.output_layout = spec.at("layout").get<std::string>();
        config.renderer.sofa_path = sofa_path;
        config.renderer.binaural_spread_mode =
            spec.at("cloud").get<bool>() ? BinauralSpreadMode::cloud : BinauralSpreadMode::none;
        config.renderer.sample_rate = spec.at("input_rate");
        config.output_sample_rate = spec.at("output_rate");
        config.startup_watermark_frames = 1;
        config.output_ring_frames = 8192;
        config.clock = [this] {
            return Clock::time_point(
                std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(time_ns.load())));
        };
        stream = std::shared_ptr<SceneStreamEngine>(take(SceneStreamEngine::create(config)));
        channels = stream->output_format().channels;
        if (device_dsp) {
            require(channels == 2, "device DSP needs stereo");
            auto device = std::make_unique<Capture>();
            capture = device.get();
            output = take(SceneOutputSession::create_with_device(stream, std::move(device), true));
        }
        uint32_t previous_sample = 0;
        for (const auto& item : spec.at("metadata")) {
            live_scene::MetadataUpdate update;
            update.element_id = 1;
            update.offset_samples = item.at("sample");
            require(update.offset_samples > previous_sample && update.offset_samples < k_main_epoch_end,
                    "invalid replay event order");
            previous_sample = update.offset_samples;
            update.ramp_duration_samples = item.at("ramp");
            const std::string field = item.at("field");
            if (field == "gain") {
                update.changed_fields = live_scene::state_linear_gain;
                update.state.linear_gain = item.at("value");
            } else if (field == "position") {
                update.changed_fields = live_scene::state_position;
                update.state.x = item.at("value").at(0);
                update.state.y = item.at("value").at(1);
                update.state.z = item.at("value").at(2);
            } else if (field == "size") {
                update.changed_fields = live_scene::state_extent;
                update.state.width = update.state.height = update.state.depth = item.at("value");
            } else if (field == "head_locked") {
                update.changed_fields = live_scene::state_head_locked;
                update.state.head_locked = item.at("value");
            } else {
                throw std::runtime_error("unknown metadata event");
            }
            update.state.valid_fields = update.changed_fields;
            updates.push_back(update);
        }
    }
    [[nodiscard]] uint32_t channel_count() const { return channels; }
    [[nodiscard]] uint32_t sample_rate() const { return config.output_sample_rate; }
    [[nodiscard]] std::span<const float> samples() const { return captured; }
    [[nodiscard]] uint64_t underrun_count() const { return stream->status().underruns; }

  private:
    void healthy() const {
        const auto s = stream->status();
        require(!s.failed, "Scene worker failed");
        require(s.underruns == 0, "unexpected replay underrun");
        if (output) {
            require(!output->status().failed, "device session failed");
        }
    }
    void device_controls() {
        if (!output) {
            return;
        }
        const auto frames = captured.size() / channels;
        if (frames != next_device_control) {
            return;
        }
        HptfProfile profile;
        if (frames == 2048) {
            profile.preamp_db = -3;
            profile.bands.push_back({HptfBandType::peaking, true, 1700, 6, 0.75});
            done(output->set_hptf_parameters(profile, HptfPreampMode::auto_trim, 1));
        } else if (frames == 4096) {
            profile.preamp_db = -6;
            profile.bands.push_back({HptfBandType::high_shelf, true, 4200, -4, 0.75});
            done(output->set_hptf_parameters(profile, HptfPreampMode::warn_only, 2));
        } else if (frames == 6144) {
            done(output->set_hptf(std::nullopt, 3));
        } else if (frames == 8192) {
            done(output->set_volume(0.5F));
        }
        next_device_control += 2048;
    }
    bool drain(bool ending) {
        healthy();
        const auto status = stream->status();
        const bool finished = status.production_complete;
        auto available = static_cast<std::size_t>(status.buffered_output_frames);
        if (output) {
            const auto consumed = captured.size() / channels;
            require(status.media_frames_pulled >= consumed, "invalid device media accounting");
            available += static_cast<std::size_t>(status.media_frames_pulled - consumed);
            const auto lookahead = std::max<uint32_t>(1, config.output_sample_rate / 200);
            if (!finished) {
                available = available > lookahead ? available - lookahead : 0;
            }
        }
        if (available == 0 && !(ending && finished)) {
            return false;
        }
        device_controls();
        std::size_t count = std::min<std::size_t>(512, available);
        if (output && count != 0) {
            count = std::min(count, next_device_control - (captured.size() / channels));
        }
        // A one-frame read is permitted only after production completes, to observe EOS.
        if (count == 0) {
            count = 1;
        }
        std::vector<float> data(count * channels);
        std::size_t got = 0;
        if (output) {
            got = capture->pull(data);
            ended = output->status().ended;
        } else {
            const auto result = stream->pull(data.data(), static_cast<uint32_t>(count));
            require((result.flags & (scene_pull_failed | scene_pull_underrun)) == 0, "invalid replay pull flags");
            got = result.media_frames;
            ended = (result.flags & scene_pull_eos) != 0;
        }
        require(got == count || finished, "short replay pull before EOS");
        captured.insert(captured.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(got * channels));
        healthy();
        return got != 0 || ended;
    }
    void fence() {
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (stream->status().queued_input_samples != 0) {
            (void) drain(false);
            healthy();
            require(Clock::now() < deadline, "replay input processing timed out");
            std::this_thread::yield();
        }
        done(stream->wait_idle(std::chrono::seconds(20)));
        while (drain(false)) {}
    }
    bool
    controls(uint64_t epoch, uint32_t position, uint64_t& generation, const live_scene::ElementDescriptor& element) {
        bool reconfigure = false;
        if (epoch == 1) {
            if (position == 2048) {
                stream->set_listener_orientation({37, 23, -19});
            }
            if (position == 4096) {
                ++generation;
                done(stream->configure_generation(epoch, generation, std::span{&element, 1U}));
                reconfigure = true;
            }
            if (position == 6144) {
                done(stream->set_semantic_policy_json(
                    R"({"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5}}})", 99));
            }
            if (position == 8192 && config.renderer.renderer == RendererSelection::triple_balance) {
                auto next = config.renderer;
                next.renderer = RendererSelection::saf;
                if (next.output_layout == "7.1.4") {
                    next.output_layout = "4+7+0";
                }
                if (next.output_layout == "22.2") {
                    next.output_layout = "9+10+3";
                }
                done(stream->switch_backend(next));
            }
            if (position == 8192 && channels == 2) {
                auto next = config.renderer;
                if (spec.contains("sofa")) {
                    next.sofa_path.clear();
                } else {
                    next.renderer = next.renderer == RendererSelection::saf ? RendererSelection::saf_binaural
                                                                            : RendererSelection::saf;
                    next.output_layout = next.renderer == RendererSelection::saf ? "0+2+0" : "binaural";
                }
                done(stream->switch_backend(next));
            }
        }
        return reconfigure;
    }

  public:
    // NOLINTNEXTLINE(readability-function-size): one complete replay transaction including timeline and EOS checks.
    void run_epoch(uint64_t epoch, uint32_t target, uint32_t end) {
        captured.clear();
        ended = false;
        next_device_control = 2048;
        time_ns.store(static_cast<int64_t>(epoch) * 2000000000LL);
        if (output) {
            done(output->begin_epoch(epoch, target));
            done(output->set_hptf(std::nullopt, 0));
            done(output->set_volume(0.875F));
            output->play();
        } else {
            done(stream->begin_epoch(epoch, target));
        }
        done(stream->set_semantic_policy_json("", epoch * 10));
        done(stream->switch_backend(config.renderer));
        done(stream->wait_idle(std::chrono::seconds(20)));
        live_scene::ElementDescriptor element;
        element.element_id = 1;
        element.role = live_scene::ElementRole::object;
        uint64_t generation = 1;
        done(stream->configure_generation(epoch, generation, std::span{&element, 1U}));
        live_scene::StateEntry initial;
        initial.element_id = 1;
        initial.state.valid_fields = live_scene::k_known_state_fields;
        initial.state.x = 0.25F;
        initial.state.y = 0.75F;
        initial.state.z = 0.125F;
        initial.state.width = spec.at("cloud").get<bool>() ? 0.5F : 0;
        initial.state.height = initial.state.width / 2;
        initial.state.diffuse = spec.at("cloud").get<bool>() ? 0.25F : 0;
        if (config.renderer.renderer == RendererSelection::triple_balance) {
            initial.state.height = initial.state.depth = initial.state.width;
            initial.state.diffuse = 0;
        }
        const auto source = phase2::signal(end, k_signal_seed, signal_scale(device_dsp));
        const std::vector<uint32_t> chunks = spec.at("partition").get<std::vector<uint32_t>>();
        require(!chunks.empty() && std::ranges::all_of(chunks, [](uint32_t n) { return n > 0 && n <= 1024; }),
                "invalid replay partition");
        std::size_t step = 0;
        bool complete = true;
        for (uint32_t position = 0; position < end;) {
            fence();
            time_ns.store((static_cast<int64_t>(epoch) * 2000000000LL) +
                          ((static_cast<int64_t>(position) * 1000000000LL) / config.renderer.sample_rate) +
                          (position >= 10240 ? 1000000000LL : 0LL));
            complete = controls(epoch, position, generation, element) || complete;
            done(stream->wait_idle(std::chrono::seconds(20)));
            uint32_t count = std::min(chunks.at(step++ % chunks.size()), end - position);
            require(count > 0, "empty partition");
            for (uint32_t boundary : {target, 2048U, 4096U, 6144U, 8192U, 10240U}) {
                if (boundary > position) {
                    count = std::min(count, boundary - position);
                }
            }
            std::vector<live_scene::MetadataUpdate> frame_updates;
            if (epoch == 1) {
                for (auto update : updates) {
                    if (update.offset_samples >= position && update.offset_samples < position + count) {
                        update.offset_samples -= position;
                        frame_updates.push_back(update);
                    }
                }
            }
            ScenePcmPlaneView plane{1, source.data() + position, count, 1, true};
            SceneFrameView frame;
            frame.epoch_id = epoch;
            frame.generation_id = generation;
            frame.media_sample_start = position;
            frame.duration_samples = count;
            // Completeness describes the retained semantic state, not whether this
            // frame carries a new initial-state snapshot. Clearing it mutes audio.
            frame.flags = live_scene::frame_state_complete;
            frame.pcm = std::span{&plane, 1U};
            frame.initial_states = complete ? std::span{&initial, 1U} : std::span<live_scene::StateEntry>{};
            frame.updates = frame_updates;
            require(take(stream->submit_frame(frame, std::chrono::seconds(20))) == SceneSubmitStatus::accepted,
                    "replay submit rejected");
            complete = false;
            position += count;
        }
        fence();
        done(stream->signal_end(epoch, end));
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (!ended) {
            (void) drain(true);
            require(Clock::now() < deadline, "replay EOS timed out");
            std::this_thread::yield();
        }
        healthy();
        const auto expected =
            (static_cast<uint64_t>(end - target) * config.output_sample_rate + config.renderer.sample_rate - 1) /
            config.renderer.sample_rate;
        require(captured.size() == expected * channels, "replay rational frame count mismatch");
        if (epoch == 1) {
            for (uint32_t input_start : {1024U, 3072U, 6144U, 10240U}) {
                const auto first = static_cast<std::size_t>(static_cast<uint64_t>(input_start) *
                                                            config.output_sample_rate / config.renderer.sample_rate);
                const auto last = static_cast<std::size_t>(static_cast<uint64_t>(input_start + 512U) *
                                                           config.output_sample_rate / config.renderer.sample_rate);
                const auto window =
                    std::span<const float>{captured}.subspan(first * channels, (last - first) * channels);
                if (!std::ranges::any_of(window, [](float value) { return std::abs(value) > 1.0e-9F; })) {
                    throw std::runtime_error("active replay interval unexpectedly silent at input sample " +
                                             std::to_string(input_start));
                }
            }
        }
    }

  private:
    Json spec;
    SceneStreamConfig config;
    std::atomic<int64_t> time_ns{1000000000LL};
    std::shared_ptr<SceneStreamEngine> stream;
    std::unique_ptr<SceneOutputSession> output;
    Capture* capture{nullptr};
    std::vector<live_scene::MetadataUpdate> updates;
    std::vector<float> captured;
    uint32_t channels{0};
    std::size_t next_device_control{2048};
    bool ended{false}, device_dsp{false};
};

void verify_controls() {
    std::atomic<unsigned> clock_reads{0};
    SceneStreamConfig config;
    config.renderer.output_layout = "0+2+0";
    config.output_ring_frames = 16;
    config.startup_watermark_frames = 1;
    config.clock = [&clock_reads] {
        clock_reads.fetch_add(1);
        return Clock::time_point(std::chrono::seconds(1));
    };
    auto stream = take(SceneStreamEngine::create(config));
    done(stream->begin_epoch(1, 0));
    live_scene::ElementDescriptor element;
    element.element_id = 1;
    done(stream->configure_generation(1, 1, std::span{&element, 1U}));
    stream->set_listener_orientation({1, 0, 0});
    std::vector<float> input(64, 0.125F);
    ScenePcmPlaneView plane{1, input.data(), 64, 1, true};
    live_scene::StateEntry initial;
    initial.element_id = 1;
    initial.state.valid_fields = live_scene::k_known_state_fields;
    SceneFrameView frame;
    frame.epoch_id = 1;
    frame.generation_id = 1;
    frame.duration_samples = 64;
    frame.flags = live_scene::frame_state_complete;
    frame.pcm = std::span{&plane, 1U};
    frame.initial_states = std::span{&initial, 1U};
    require(take(stream->submit_frame(frame, std::chrono::milliseconds(0))) == SceneSubmitStatus::accepted,
            "control test submit");
    const auto timed_out = stream->wait_idle(std::chrono::milliseconds(10));
    require(!timed_out && timed_out.error().message.find("timed out") != std::string::npos && !stream->status().failed,
            "fence must time out behind full ring");
    require(clock_reads.load() > 0, "injected clock was not used");
    done(stream->begin_epoch(2, 0));
    done(stream->wait_idle(std::chrono::seconds(5)));
    require(!stream->status().failed && stream->status().queued_input_samples == 0, "fence/reset failed to recover");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--sofa-supported") {
            std::cout << (binaural_sofa_supported() ? "yes\n" : "no\n");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            verify_controls();
            return 0;
        }
        require(argc == 3, "usage: mr_adm_phase2_scene case.json output-directory");
        std::ifstream in(argv[1]);
        const auto spec = Json::parse(in);
        validate_spec(spec);
        std::filesystem::path sofa_path;
        if (spec.contains("sofa")) {
            require(binaural_sofa_supported(), "SOFA replay is disabled in this build");
            const std::filesystem::path name(spec.at("sofa").get<std::string>());
            require(name == name.filename() && name.extension() == ".sofa", "invalid replay SOFA fixture name");
            sofa_path = std::filesystem::path(argv[1]).parent_path().parent_path() / "fixtures" / name;
            require(std::filesystem::is_regular_file(sofa_path), "missing replay SOFA fixture");
        }
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        save_json(directory / "events.json", spec);
        const auto input = phase2::signal(k_main_epoch_end, k_signal_seed, signal_scale(spec.at("device_dsp")));
        phase2::pcm(directory / "input.pcmbits", 1, spec.at("input_rate"), input);
        for (int pass = 1; pass <= 2; ++pass) {
            // A second replay can use different callback segment boundaries even
            // with identical PCM. Never mix its checkpoints into the first pass.
            if (pass == 2) {
#ifdef _WIN32
                require(_putenv_s("MR_ADM_TRACE_DIR", "") == 0, "cannot disable repeat trace");
#else
                require(unsetenv("MR_ADM_TRACE_DIR") == 0, "cannot disable repeat trace");
#endif
            }
            Replay replay(spec, sofa_path);
            Json lengths = Json::array();
            for (uint64_t epoch : {1U, 2U}) {
                replay.run_epoch(epoch, epoch == 1 ? 0U : k_seek_target, epoch == 1 ? k_main_epoch_end : k_seek_end);
                const std::string name = "pass-" + std::to_string(pass) + "-epoch-" + std::to_string(epoch);
                phase2::pcm(
                    directory / (name + ".pcmbits"), replay.channel_count(), replay.sample_rate(), replay.samples());
                lengths.push_back({{"epoch", epoch}, {"frames", replay.samples().size() / replay.channel_count()}});
            }
            save_json(directory / ("pass-" + std::to_string(pass) + ".json"),
                      {{"epochs", lengths}, {"underruns", replay.underrun_count()}, {"status", "passed"}});
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
