#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/monitor.h"
#include "adm/render.h"

#include "../adm_core/direct_matrix_records.h"
#include "../adm_render_room/room_internal.h"
#include "audio_output_device.h"
#include "catalog.h"
#include "hptf_eq.h"
#include "island_stream.h"
#include "render_common.h"
#include "renderer_factory.h"
#include "request_record.h"
#include "scene_handle.h"

// The session logic (log buffer, stream opening, options, edits, HpTF, backend and device
// transactions) is Rust mradm-engine `monitor::session` (PCM consistency 5.5 B). This shell keeps
// the devices, the Apple island and the RenderOptions each backend choice was made from.
namespace mradm {

namespace {

constexpr int32_t k_thrown = 100;

[[nodiscard]] std::unique_ptr<realtime::IAudioOutputDevice> make_monitor_device(const RenderOptions& options,
                                                                                const std::string& device_id) {
#ifdef __APPLE__
    // System-spatial monitor follows the system media route (AirPods / current output), not a
    // selectable raw hardware token. Keep create() and set_output_device() on the same path so a
    // device refresh cannot silently fall back to miniaudio and lose system head tracking.
    if (options.monitor_system_spatial) {
        return realtime::make_avsamplebuffer_device(normalize_output_layout(options.output_layout));
    }
#elif defined(_WIN32)
    // Windows system-spatial: hand the bed to ISpatialAudioClient so the active spatializer (Windows
    // Sonic / Dolby Atmos / DTS for headphones) renders it. No OS head tracking (static), so unlike
    // macOS there is no per-route reason to pin this — but keep it on the same selection path so a
    // device refresh can't silently drop to miniaudio and lose spatialization.
    if (options.monitor_system_spatial) {
        return realtime::make_spatialaudioclient_device(normalize_output_layout(options.output_layout),
                                                        options.speaker_geometry);
    }
#else
    (void) options;
#endif
    return realtime::make_miniaudio_device(/*use_null_backend=*/false, device_id);
}

RenderOptions& options_of(void* ctx) {
    return *static_cast<RenderOptions*>(ctx);
}

realtime::IAudioOutputDevice& device_of(void* device) {
    return *static_cast<realtime::IAudioOutputDevice*>(device);
}

// An exception becomes code 100 (the session rethrows it).
template <class F> int32_t guarded(const MradmEngineIslandError* e, const F& f) noexcept {
    try {
        return f();
    } catch (const std::exception& error) {
        return realtime::island_error(Error{static_cast<ErrorCode>(k_thrown), error.what(), {}}, e);
    } catch (...) {
        return realtime::island_error(Error{static_cast<ErrorCode>(k_thrown), "unknown exception", {}}, e);
    }
}

// Both members retain the Apple backend for as long as its stream lives.
struct AppleKeepAlive {
    std::unique_ptr<IRenderer> renderer; // cppcheck-suppress unusedStructMember
    // cppcheck-suppress unusedStructMember
    std::shared_ptr<IPreparedRender> prepared;
};

// The error buffers of one Rust session call (large enough for any path in a message).
constexpr size_t k_text_capacity = 65536;
class Texts {
  public:
    explicit Texts(size_t context_capacity = k_text_capacity)
        : context_(std::max(context_capacity, k_text_capacity), '\0'), context_len_(context_.size()) {}
    uint8_t* m() { return reinterpret_cast<uint8_t*>(message_.data()); }
    uint8_t* c() { return reinterpret_cast<uint8_t*>(context_.data()); }
    size_t* ml() { return &message_len_; }
    size_t* cl() { return &context_len_; }

    [[nodiscard]] Result<void> result(int32_t code) {
        if (code == 0) {
            return {};
        }
        message_.resize(message_len_);
        context_.resize(context_len_);
        if (code == k_thrown) {
            throw std::runtime_error(message_);
        }
        return make_error(static_cast<ErrorCode>(code), std::move(message_), std::move(context_));
    }

  private:
    std::string message_ = std::string(k_text_capacity, '\0');
    std::string context_;
    size_t message_len_ = k_text_capacity;
    size_t context_len_ = 0;
};

// The import's log lines, replayed into the session once it exists.
class BufferedLogSink final : public LogSink {
  public:
    void log(LogLevel level, std::string_view module, std::string_view message) override {
        lines_.push_back({level, std::string{module}, std::string{message}});
    }
    void replay(LogSink& sink) const {
        for (const auto& line : lines_) {
            sink.log(line.level, line.module, line.message);
        }
    }

  private:
    struct Line {
        LogLevel level{};
        std::string module;
        std::string message;
    };
    std::vector<Line> lines_;
};

} // namespace

// The scene and its handle outlive the Rust session, which borrows the handle and opens Apple
// streams on the scene.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct MonitorSession::Impl {
    std::string input_path;
    AdmScene scene;
    std::shared_ptr<const detail::RenderSceneHandle> scene_handle;
    MradmEngineMonitorSessionHost host{};
    MradmEngineMonitorSession* rust{nullptr};

    ~Impl() { mradm_engine_monitor_session_destroy(rust); }
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Result<MradmEngineIslandStream>
    open_apple(const RenderOptions& options, std::string layout, const MradmDirectMatrixView* matrix) const;
    void bind_host();
};

namespace {

// Forwards to the session's log buffer (the import's and the Apple backend's lines).
class SessionLogSink final : public LogSink {
  public:
    explicit SessionLogSink(const MradmEngineMonitorSession* session) : session_(session) {}
    void log(LogLevel level, std::string_view module, std::string_view message) override {
        mradm_engine_monitor_session_log(session_,
                                         static_cast<uint32_t>(level),
                                         reinterpret_cast<const uint8_t*>(module.data()),
                                         module.size(),
                                         reinterpret_cast<const uint8_t*>(message.data()),
                                         message.size());
    }

  private:
    const MradmEngineMonitorSession* session_;
};

} // namespace

// NOLINTEND(misc-non-private-member-variables-in-classes)

// The Apple island over the engine's resolved layout and DirectSpeakers matrix; the stream owns
// the renderer and its prepared state.
Result<MradmEngineIslandStream> MonitorSession::Impl::open_apple(const RenderOptions& options,
                                                                 std::string layout,
                                                                 const MradmDirectMatrixView* matrix) const {
    auto renderer = engine::apple_renderer();
    if (!renderer) {
        return tl::unexpected{renderer.error()};
    }
    RenderPlan plan;
    plan.input_path = input_path;
    plan.output_layout = std::move(layout);
    plan.scene = scene;
    plan.scene_handle = scene_handle;
    plan.sofa_path = options.sofa_path;
    plan.room = options.room;
    plan.screens = options.screens;
    plan.default_interp_ms = options.default_interp_ms;
    plan.object_smoothing_frames = options.object_smoothing_frames;
    plan.speaker_geometry = options.speaker_geometry;
    plan.direct_speakers_routing_mode = options.direct_speakers_routing_mode;
    if (matrix != nullptr) {
        plan.direct_speakers_matrix = std::make_shared<const DirectSpeakersMatrix>(detail::matrix_from_view(*matrix));
    }
    plan.speaker_spread_mode = options.speaker_spread_mode;
    plan.triple_balance_mode = options.triple_balance_mode;
    plan.binaural_spread_mode = options.binaural_spread_mode;
    plan.lfe_routing_mode = options.lfe_routing_mode;
    plan.apple_spatial_preset = options.apple_spatial_preset;
    plan.apple_speaker_rendering_flags = options.apple_speaker_rendering_flags;
    // System-spatial monitoring hands head tracking to the system; the rendered bed must stay
    // orientation-neutral so it isn't rotated twice (see MonitorSession::set_listener_orientation).
    plan.listener_orientation = options.monitor_system_spatial ? ListenerOrientation{} : options.listener_orientation;
    SessionLogSink logs(rust);
    auto prepared = (*renderer)->prepare(plan, logs);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    std::shared_ptr<IPreparedRender> prepared_state = std::move(*prepared);
    auto stream = (*renderer)->open_stream(*prepared_state, plan, logs);
    if (!stream) {
        return tl::unexpected{stream.error()};
    }
    auto keep = std::make_shared<AppleKeepAlive>(AppleKeepAlive{std::move(*renderer), std::move(prepared_state)});
    return realtime::island_table(std::move(*stream), std::move(keep));
}

void MonitorSession::Impl::bind_host() {
    host.ctx = this;
    host.device = [](void*, void* options, const uint8_t* id, size_t id_len) noexcept -> void* {
        try {
            return make_monitor_device(options_of(options), {reinterpret_cast<const char*>(id), id_len}).release();
        } catch (...) {
            std::terminate();
        }
    };
    host.device_realtime = [](void*, void* device) noexcept -> uint32_t {
        return device_of(device).pull_is_realtime_playback() ? 1U : 0U;
    };
    host.device_start = [](void*,
                           void* device,
                           uint32_t channels,
                           uint32_t rate,
                           const MradmEngineSessionMonitor* monitor,
                           const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] {
            auto& d = device_of(device);
            auto started = d.start(channels, rate, [monitor, &d](std::span<float> out, std::size_t frames) {
                MradmEngineLiveMonitorPull result{};
                mradm_engine_session_monitor_pull(monitor, out.data(), out.size(), frames, &result);
                if (result.mark_end != 0U) {
                    d.mark_end();
                }
                return result.frames;
            });
            return started ? 0 : realtime::island_error(started.error(), e);
        });
    };
    host.device_stop = [](void*, void* device) noexcept { device_of(device).stop(); };
    host.device_flush = [](void*, void* device) noexcept { device_of(device).flush(); };
    host.device_pause = [](void*, void* device) noexcept { device_of(device).pause(); };
    host.device_resume = [](void*, void* device) noexcept { device_of(device).resume(); };
    host.device_destroy = [](void*, void* device) noexcept { delete &device_of(device); };
    host.open_apple = [](void* ctx,
                         void* options,
                         const uint8_t* layout,
                         size_t layout_len,
                         const MradmDirectMatrixView* matrix,
                         MradmEngineIslandStream* out,
                         const MradmEngineIslandError* e) noexcept {
        return guarded(e, [&] {
            const auto& impl = *static_cast<const Impl*>(ctx);
            auto table = impl.open_apple(
                options_of(options), std::string{reinterpret_cast<const char*>(layout), layout_len}, matrix);
            if (!table) {
                return realtime::island_error(table.error(), e);
            }
            *out = *table;
            return 0;
        });
    };
    host.release_options = [](void*, void* options) noexcept { delete &options_of(options); };
}

MonitorSession::MonitorSession() : impl_(std::make_unique<Impl>()) {}
MonitorSession::~MonitorSession() = default;

std::vector<AudioOutputDeviceInfo> MonitorSession::list_output_devices() {
    std::vector<AudioOutputDeviceInfo> out;
    const auto devices = realtime::enumerate_output_devices();
    out.reserve(devices.size());
    std::ranges::transform(devices, std::back_inserter(out), [](const realtime::AudioDeviceInfo& d) {
        return AudioOutputDeviceInfo{d.id, d.name, d.is_default};
    });
    return out;
}

Result<std::unique_ptr<MonitorSession>>
MonitorSession::create(const std::string& input_path, const RenderOptions& options, const std::string& device_id) {
    std::unique_ptr<MonitorSession> session{new MonitorSession()};
    Impl& impl = *session->impl_;

    // Import + apply the semantic policy via the same path PreviewSession uses, so the
    // monitored scene is the effective (policy-applied) scene — matching offline rendering.
    RenderService service;
    BufferedLogSink import_logs;
    auto scene = service.prepare_preview_scene(input_path, options, import_logs);
    if (!scene) {
        return tl::unexpected{scene.error()};
    }
    impl.input_path = input_path;
    impl.scene = std::move(scene->scene);
    impl.scene_handle = std::move(scene->handle);
    impl.bind_host();
    const engine::RenderPlatform platform;
    impl.rust = mradm_engine_monitor_session_new(platform.get(),
                                                 impl.scene_handle != nullptr ? impl.scene_handle->raw : nullptr,
                                                 impl.scene.info.sample_rate,
                                                 impl.scene.info.num_frames,
                                                 &impl.host);
    if (impl.rust == nullptr) {
        return make_error(ErrorCode::internal_error, "invalid monitor session");
    }
    SessionLogSink logs(impl.rust);
    import_logs.replay(logs);

    const RenderRequest request{input_path, std::nullopt, options};
    const engine::RequestTexts texts(request);
    const auto record = texts.record(request);
    Texts errors;
    const auto code = mradm_engine_monitor_session_start(impl.rust,
                                                         &record,
                                                         options.monitor_system_spatial ? 1U : 0U,
                                                         new RenderOptions(options),
                                                         reinterpret_cast<const uint8_t*>(device_id.data()),
                                                         device_id.size(),
                                                         errors.m(),
                                                         errors.ml(),
                                                         errors.c(),
                                                         errors.cl());
    if (auto created = errors.result(code); !created) {
        return tl::unexpected{created.error()};
    }
    return session;
}

void MonitorSession::play() {
    mradm_engine_monitor_session_play(impl_->rust);
}

void MonitorSession::pause() {
    mradm_engine_monitor_session_pause(impl_->rust);
}

Result<void> MonitorSession::seek_seconds(double seconds) {
    Texts errors;
    return errors.result(
        mradm_engine_monitor_session_seek(impl_->rust, seconds, errors.m(), errors.ml(), errors.c(), errors.cl()));
}

void MonitorSession::set_loop_seconds(double start_seconds, double end_seconds) {
    mradm_engine_monitor_session_set_loop(impl_->rust, start_seconds, end_seconds);
}

Result<void> MonitorSession::set_overrides(const LiveOverrides& overrides) {
    const auto records = render_common::live_override_records(overrides);
    size_t context = 0;
    for (const auto& item : overrides.objects) {
        context = std::max(context, item.object_id.size() + 1);
    }
    Texts errors(context);
    return errors.result(mradm_engine_monitor_session_overrides(impl_->rust,
                                                                records.data(),
                                                                records.size(),
                                                                overrides.revision,
                                                                errors.m(),
                                                                errors.ml(),
                                                                errors.c(),
                                                                errors.cl()));
}

std::shared_ptr<IRoomControl> MonitorSession::room_control() const {
    auto* raw = mradm_engine_monitor_session_room_control(impl_->rust);
    return raw == nullptr ? nullptr : room_internal::control(raw);
}

void MonitorSession::set_listener_orientation(const ListenerOrientation& orientation) {
    mradm_engine_monitor_session_orientation(
        impl_->rust, orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg);
}

Result<void> MonitorSession::set_hptf(const std::string& path, HptfPreampMode mode, std::uint64_t revision) {
    auto settings = render_common::prepare_hptf(path, mode, revision);
    return settings ? set_hptf_prepared(*settings) : tl::unexpected{settings.error()};
}
Result<void>
MonitorSession::set_hptf_parameters(const HptfProfile& profile, HptfPreampMode mode, std::uint64_t revision) {
    auto settings = render_common::prepare_hptf(profile, mode, revision);
    return settings ? set_hptf_prepared(*settings) : tl::unexpected{settings.error()};
}
Result<void> MonitorSession::set_hptf_prepared(const render_common::HptfSettings& settings) {
    Texts errors;
    return errors.result(mradm_engine_monitor_session_hptf(
        impl_->rust, settings.handle.get(), errors.m(), errors.ml(), errors.c(), errors.cl()));
}
HptfStereoInfo MonitorSession::hptf_stereo_info() const {
    MradmDspHptfSnapshot snapshot{};
    if (mradm_engine_monitor_session_hptf_snapshot(impl_->rust, &snapshot) == 0U) {
        return {};
    }
    return render_common::hptf_snapshot_info(render_common::hptf_snapshot(snapshot));
}
HptfInfo MonitorSession::hptf_info() const {
    return hptf_stereo_info().left;
}

Result<void> MonitorSession::switch_backend(const RenderOptions& options) {
    const RenderRequest request{impl_->input_path, std::nullopt, options};
    const engine::RequestTexts texts(request);
    const auto record = texts.record(request);
    Texts errors;
    return errors.result(mradm_engine_monitor_session_switch(impl_->rust,
                                                             &record,
                                                             options.monitor_system_spatial ? 1U : 0U,
                                                             new RenderOptions(options),
                                                             errors.m(),
                                                             errors.ml(),
                                                             errors.c(),
                                                             errors.cl()));
}

Result<void> MonitorSession::set_output_device(const std::string& device_id) {
    Texts errors;
    return errors.result(mradm_engine_monitor_session_output_device(impl_->rust,
                                                                    reinterpret_cast<const uint8_t*>(device_id.data()),
                                                                    device_id.size(),
                                                                    errors.m(),
                                                                    errors.ml(),
                                                                    errors.c(),
                                                                    errors.cl()));
}

MonitorStatusSnapshot MonitorSession::status() const {
    MradmEngineLiveMonitorStatus s{};
    MonitorStatusSnapshot out;
    if (mradm_engine_monitor_session_status(impl_->rust, &s) == 0U) {
        out.state = MonitorPlaybackState::stopped; // invalid session → a stopped/failed snapshot
        out.failed = true;
        return out;
    }
    out.state = static_cast<MonitorPlaybackState>(s.state);
    out.playhead_frames = s.playhead_frames;
    out.underruns = s.underruns;
    out.buffered_frames = s.buffered_frames;
    out.ring_fill = s.ring_fill;
    out.ended = s.ended != 0U;
    out.failed = s.failed != 0U;
    out.override_revision = s.override_revision;
    return out;
}

MonitorLevelsSnapshot MonitorSession::levels() const {
    MradmEngineLiveMonitorLevels l{};
    MonitorLevelsSnapshot out;
    if (mradm_engine_monitor_session_levels(impl_->rust, &l) == 0U) {
        return out; // invalid session → silent levels
    }
    out.channels = l.channels;
    std::copy_n(std::begin(l.peak), out.peak.size(), out.peak.begin());
    std::copy_n(std::begin(l.rms), out.rms.size(), out.rms.begin());
    out.momentary_lufs = l.momentary_lufs;
    out.shortterm_lufs = l.shortterm_lufs;
    out.integrated_lufs = l.integrated_lufs;
    return out;
}

std::size_t MonitorSession::log_count() const {
    return mradm_engine_monitor_session_log_count(impl_->rust);
}

bool MonitorSession::log_entry(std::size_t index, LogLevel& level, std::string& module, std::string& message) const {
    uint32_t value = 0;
    MradmAdmText m{};
    MradmAdmText t{};
    if (mradm_engine_monitor_session_log_entry(impl_->rust, index, &value, &m, &t) == 0U) {
        return false;
    }
    level = static_cast<LogLevel>(value);
    module = render_common::engine_text(m);
    message = render_common::engine_text(t);
    return true;
}

} // namespace mradm
