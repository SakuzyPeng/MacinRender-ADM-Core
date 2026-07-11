#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/monitor.h"
#include "adm/render.h"

#include "audio_output_device.h"
#include "downmix_stream.h"
#include "monitor_engine.h"
#include "render_stream_factory.h"
#include "renderer_factory.h"
#include "speaker_layouts.h"

namespace mradm {

namespace {

// Seconds → absolute output frame, clamped to [0, total_frames]. Compares against the
// material duration before casting so an arbitrarily large (but finite) `seconds` never
// produces an out-of-range double→uint64_t conversion (which is UB). NaN maps to 0.
[[nodiscard]] uint64_t clamp_frame(double seconds, uint32_t sample_rate, uint64_t total_frames) {
    if (!(seconds > 0.0) || sample_rate == 0) {
        return 0;
    }
    const double duration_sec = static_cast<double>(total_frames) / static_cast<double>(sample_rate);
    if (seconds >= duration_sec) {
        return total_frames;
    }
    // seconds < duration ⇒ seconds * sample_rate < total_frames, so the cast is in range.
    return static_cast<uint64_t>(seconds * static_cast<double>(sample_rate));
}

// Constant-power stereo fold of one source speaker at `azimuth` (degrees, +ve = left,
// BS.2051). az is clamped to ±90° so rear speakers fold to the nearest side. Returns the
// {L, R} gains.
[[nodiscard]] std::pair<float, float> stereo_pan_for_azimuth(float azimuth) {
    const float az = std::clamp(azimuth, -90.0F, 90.0F);
    const float a = ((az / 90.0F) + 1.0F) * 0.5F; // 0 = hard right, 0.5 = center, 1 = hard left
    const float angle = a * (std::numbers::pi_v<float> / 2.0F);
    return {std::sin(angle), std::cos(angle)};
}

// Build a downmix / upmix matrix [monitor_channels × src_channels] (row-major) mapping a
// source layout to the fixed monitor output. Returns nullopt when the conversion is not
// supported (cross-format to a non-stereo monitor). Equal channel counts → identity.
// Stereo monitor: speaker layouts fold by geometry (LFE → both at −6 dB), HOA3 uses a
// first-order horizontal W/Y decode, plain stereo passes through.
[[nodiscard]] std::optional<std::vector<float>>
build_downmix_matrix(uint32_t src_channels, std::string_view src_layout, uint32_t monitor_channels) {
    if (src_channels == 0 || monitor_channels == 0) {
        return std::nullopt;
    }
    if (src_channels == monitor_channels) {
        std::vector<float> identity(static_cast<std::size_t>(monitor_channels) * src_channels, 0.0F);
        for (uint32_t c = 0; c < monitor_channels; ++c) {
            identity[(static_cast<std::size_t>(c) * src_channels) + c] = 1.0F;
        }
        return identity;
    }
    if (monitor_channels != 2) {
        return std::nullopt; // only stereo monitor folds are implemented
    }

    std::vector<float> m(std::size_t{2U} * src_channels, 0.0F);
    auto set_lr = [&](uint32_t s, float l, float r) {
        m[s] = l;                // L row
        m[src_channels + s] = r; // R row
    };

    if (src_layout == "hoa3" && src_channels >= 4) {
        // ACN/SN3D: W = ch0, Y(left) = ch1. First-order horizontal stereo decode.
        set_lr(0, 0.7071F, 0.7071F); // W → both
        set_lr(1, 0.5F, -0.5F);      // Y → L positive, R negative
        return m;
    }
    if (const auto* layout = render_layouts::find_speaker_layout(src_layout);
        layout != nullptr && layout->speakers.size() == src_channels) {
        for (uint32_t s = 0; s < src_channels; ++s) {
            const auto& spk = layout->speakers[s];
            if (spk.is_lfe) {
                set_lr(s, 0.5F, 0.5F); // fold LFE into both at −6 dB
                continue;
            }
            const auto [l, r] = stereo_pan_for_azimuth(spk.azimuth);
            set_lr(s, l, r);
        }
        return m;
    }
    return std::nullopt; // unknown source layout → cannot fold safely
}

// Thread-safe, append-only diagnostics buffer. The monitor renders on a worker thread and
// resolves the backend at create time; this captures their warnings/errors for polling.
class BufferingLogSink final : public LogSink {
  public:
    void log(LogLevel level, std::string_view module, std::string_view message) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back({level, std::string{module}, std::string{message}});
    }

    [[nodiscard]] std::size_t count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    [[nodiscard]] bool get(std::size_t index, LogLevel& level, std::string& module, std::string& message) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (index >= entries_.size()) {
            return false;
        }
        const auto& entry = entries_[index];
        level = entry.level;
        module = entry.module;
        message = entry.message;
        return true;
    }

  private:
    struct Entry {
        LogLevel level{};
        std::string module;
        std::string message;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

// Production stream factory: resolve the backend, build a RenderPlan from the scene +
// options, prepare it, and open a realtime stream. Retains every backend's renderer +
// prepared state for the session's lifetime: streams borrow the prepared state by
// reference, and during a hot-switch crossfade two streams (outgoing + incoming) are live
// at once, so neither prepared may be freed while a stream still references it.
class RealtimeStreamFactory final : public realtime::IRenderStreamFactory {
  public:
    explicit RealtimeStreamFactory(std::string input_path) : input_path_(std::move(input_path)) {}

    Result<std::unique_ptr<IRenderStream>>
    open(const AdmScene& scene, const RenderOptions& options, LogSink& logs) override {
        const std::string requested_layout =
            normalize_output_layout(options.output_layout.empty() ? std::string{"0+2+0"} : options.output_layout);
        auto resolved = resolve_renderer(options.renderer, requested_layout, options.internal_allow_speaker_stereo);
        if (!resolved) {
            return tl::unexpected{resolved.error()};
        }
        for (const auto& [level, message] : resolved->diagnostics) {
            logs.log(level, "monitor", message);
        }
        std::unique_ptr<IRenderer> renderer = std::move(resolved->renderer);

        RenderPlan plan;
        plan.input_path = input_path_;
        plan.output_layout = resolved->effective_output_layout;
        plan.scene = scene;
        plan.sofa_path = options.sofa_path;
        plan.default_interp_ms = options.default_interp_ms;
        plan.object_smoothing_frames = options.object_smoothing_frames;
        plan.speaker_spread_mode = options.speaker_spread_mode;
        plan.binaural_spread_mode = options.binaural_spread_mode;
        plan.apple_spatial_preset = options.apple_spatial_preset;
        plan.apple_speaker_rendering_flags = options.apple_speaker_rendering_flags;
        // System-spatial monitoring hands head tracking to the system; the rendered bed must stay
        // orientation-neutral so it isn't rotated twice (see MonitorSession::set_listener_orientation).
        plan.listener_orientation =
            options.monitor_system_spatial ? ListenerOrientation{} : options.listener_orientation;

        auto prepared = renderer->prepare(plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        std::shared_ptr<IPreparedRender> prepared_state = std::move(*prepared);
        auto stream = renderer->open_stream(*prepared_state, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        keepalive_.push_back({std::move(renderer), std::move(prepared_state)});
        return std::move(*stream);
    }

    // Drop the most-recently retained backend. The caller MUST have already destroyed the
    // stream returned by the matching open() (streams borrow the prepared state). Used when
    // a just-opened backend is rejected (e.g. a hot-switch format mismatch) so repeated
    // rejected switches don't accumulate prepared state for the session's lifetime.
    void forget_last_backend() {
        if (!keepalive_.empty()) {
            keepalive_.pop_back();
        }
    }

  private:
    struct BackendKeepAlive {
        std::unique_ptr<IRenderer> renderer;
        // Held purely to keep the prepared render alive for the session's lifetime.
        // cppcheck-suppress unusedStructMember
        std::shared_ptr<IPreparedRender> prepared;
    };
    std::string input_path_;
    std::vector<BackendKeepAlive> keepalive_;
};

[[nodiscard]] std::unique_ptr<realtime::IAudioOutputDevice> make_monitor_device(const RenderOptions& options,
                                                                                const std::string& device_id) {
#if defined(__APPLE__)
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
        return realtime::make_spatialaudioclient_device(normalize_output_layout(options.output_layout));
    }
#else
    (void) options;
#endif
    return realtime::make_miniaudio_device(/*use_null_backend=*/false, device_id);
}

} // namespace

// Declaration order matters for teardown: the engine (which stops the device + joins the
// worker + destroys the stream) is destroyed first, then the device, then the factory whose
// prepared state the stream may reference, then the log buffer.
struct MonitorSession::Impl {
    BufferingLogSink log_sink;
    std::unique_ptr<RealtimeStreamFactory> factory;
    std::unique_ptr<realtime::IAudioOutputDevice> device;
    std::unique_ptr<realtime::MonitorEngine> engine;
    AdmScene scene;                  // policy-applied scene, reused when hot-switching the backend
    RenderOptions current_options;   // last-used options (create / switch_backend), to rebuild on device switch
    LiveOverrides current_overrides; // last-applied overrides, re-applied after a device switch
    ListenerOrientation current_orientation{}; // last-applied head orientation, re-applied after a device switch
    std::string device_id;                     // current output device token ("" = default)
    uint32_t sample_rate{48000};
    uint64_t total_frames{0};

    // The orientation actually fed to the render stream. Under system-spatial monitoring the
    // system owns head tracking, so a local orientation must NOT reach AppleStream (it would
    // rotate the 7.1.4 bed at the source → double rotation). Forced neutral there; the user's
    // real pose is still kept in current_orientation (and drives the GUI spatial view separately).
    [[nodiscard]] ListenerOrientation effective_orientation() const {
        return current_options.monitor_system_spatial ? ListenerOrientation{} : current_orientation;
    }
};

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
    auto scene = service.prepare_preview_scene(input_path, options, impl.log_sink);
    if (!scene) {
        return tl::unexpected{scene.error()};
    }
    impl.sample_rate = scene->info.sample_rate;
    impl.total_frames = scene->info.num_frames;
    impl.scene = std::move(*scene);
    impl.current_options = options;
    impl.device_id = device_id;

    impl.factory = std::make_unique<RealtimeStreamFactory>(input_path);
    impl.device = make_monitor_device(options, device_id);

    auto engine = realtime::MonitorEngine::create(*impl.factory, *impl.device, impl.scene, options, impl.log_sink);
    if (!engine) {
        return tl::unexpected{engine.error()};
    }
    impl.engine = std::move(*engine);
    return session;
}

void MonitorSession::play() {
    if (impl_->engine == nullptr) {
        return; // session invalidated (a backend rebuild failed); stay inert rather than crash
    }
    impl_->engine->play();
}

void MonitorSession::pause() {
    if (impl_->engine == nullptr) {
        return;
    }
    impl_->engine->pause();
}

Result<void> MonitorSession::seek_seconds(double seconds) {
    if (impl_->engine == nullptr) {
        return make_error(ErrorCode::internal_error, "监听会话无效:后端重建失败");
    }
    // negative clamps to 0, past the end clamps to total_frames.
    impl_->engine->seek(clamp_frame(seconds, impl_->sample_rate, impl_->total_frames));
    return {};
}

void MonitorSession::set_loop_seconds(double start_seconds, double end_seconds) {
    if (impl_->engine == nullptr) {
        return;
    }
    impl_->engine->set_loop(clamp_frame(start_seconds, impl_->sample_rate, impl_->total_frames),
                            clamp_frame(end_seconds, impl_->sample_rate, impl_->total_frames));
}

void MonitorSession::set_overrides(const LiveOverrides& overrides) {
    impl_->current_overrides = overrides; // kept so a device switch can re-apply the user's edits
    if (impl_->engine == nullptr) {
        return;
    }
    impl_->engine->set_overrides(overrides);
}

void MonitorSession::set_listener_orientation(const ListenerOrientation& orientation) {
    impl_->current_orientation = orientation; // kept so a device switch can re-apply the head pose
    if (impl_->engine == nullptr) {
        return;
    }
    // 系统空间化监听:头追由系统(AirPods)在它自己的输出级接管。若把本地朝向喂给 AppleStream
    // (AUSpatialMixer),它会在源头先把 7.1.4 声场转一道,再被系统头追转第二道 = 双重旋转。
    // 故在系统空间化下强制中立朝向(GUI 的 spatial view 视觉监视走另一条路,不受影响)。
    impl_->engine->set_listener_orientation(impl_->effective_orientation());
}

Result<void> MonitorSession::switch_backend(const RenderOptions& options) {
    if (impl_->engine == nullptr) {
        return make_error(ErrorCode::internal_error, "监听会话无效:后端重建失败");
    }
    // Prepare the new backend off the audio thread (resolve + prepare + open_stream),
    // reusing the policy-applied scene. The factory retains its prepared state.
    auto stream = impl_->factory->open(impl_->scene, options, impl_->log_sink);
    if (!stream) {
        return tl::unexpected{stream.error()};
    }
    if (*stream == nullptr) {
        return make_error(ErrorCode::internal_error, "monitor switch_backend produced a null stream");
    }
    const uint32_t monitor_channels = impl_->engine->out_channels();
    // No resampling across a switch: the monitor device runs at a fixed rate.
    if ((*stream)->sample_rate() != impl_->engine->sample_rate()) {
        stream->reset();
        impl_->factory->forget_last_backend();
        return make_error(ErrorCode::unsupported, "monitor backend switch requires the same sample rate");
    }
    // The engine's ring layout (intermediate vs output PCM) is fixed at create from the stream's
    // output-stage mode. A switch that flips that mode (e.g. Apple binaural output-stage ↔ a
    // downmixed speaker stream) can't be hot-swapped — rebuild the engine on the same device,
    // preserving the playhead / edits / play state (mirrors set_output_device).
    const bool channels_match = (*stream)->out_channels() == monitor_channels;
    const bool new_output_stage = channels_match && (*stream)->renders_orientation_at_output();
    if (new_output_stage != impl_->engine->output_stage()) {
        stream->reset(); // the factory re-opens the new backend inside MonitorEngine::create below
        const realtime::MonitorStatus snap = impl_->engine->status();
        const uint64_t playhead = snap.playhead_frames;
        const bool was_playing = snap.state == realtime::MonitorState::playing;
        const RenderOptions prev_options = impl_->current_options; // to restore the working backend on failure
        impl_->engine.reset();                                     // stops the device (engine borrows it by reference)
        impl_->device.reset();

        // Build a fresh device + engine for `opts` on the same device id. Leaves engine/device null
        // on failure (the caller decides whether to retry the previous backend).
        auto rebuild_with = [&](const RenderOptions& opts) -> Result<void> {
            impl_->factory->forget_last_backend();
            impl_->current_options = opts;
            impl_->device = make_monitor_device(opts, impl_->device_id);
            auto rebuilt =
                realtime::MonitorEngine::create(*impl_->factory, *impl_->device, impl_->scene, opts, impl_->log_sink);
            if (!rebuilt) {
                impl_->engine.reset();
                impl_->device.reset();
                return tl::unexpected{rebuilt.error()};
            }
            impl_->engine = std::move(*rebuilt);
            return {};
        };

        std::optional<Error> switch_error;
        if (auto built = rebuild_with(options); !built) {
            // Restore the previous (working) backend so monitoring survives a failed flip rather than
            // leaving the session torn down. If that also fails the session is left invalid (null
            // engine) — the entry points are null-safe.
            switch_error = built.error();
            impl_->log_sink.log(LogLevel::warning, "monitor", "切换后端失败,回退到原后端:" + built.error().message);
            if (auto restored = rebuild_with(prev_options); !restored) {
                return tl::unexpected{built.error()};
            }
        }
        impl_->engine->seek(playhead);
        impl_->engine->set_overrides(impl_->current_overrides);
        impl_->engine->set_listener_orientation(impl_->effective_orientation());
        if (was_playing) {
            impl_->engine->play();
        }
        // Reverted to the old backend: surface the original failure so the UI shows the switch failed
        // (the session is still live on the previous backend, not the requested one).
        if (switch_error.has_value()) {
            return tl::unexpected{*switch_error};
        }
        return {};
    }
    // Same channel count → hand the stream straight to the engine. Otherwise wrap it in a
    // DownmixStream that folds the new layout into the fixed monitor channel count (e.g.
    // 7.1.4 / HOA → stereo headphones), so the engine still sees a monitor-format stream.
    if ((*stream)->out_channels() != monitor_channels) {
        auto matrix = build_downmix_matrix((*stream)->out_channels(), (*stream)->output_layout(), monitor_channels);
        if (!matrix) {
            stream->reset();
            impl_->factory->forget_last_backend();
            return make_error(ErrorCode::unsupported,
                              "monitor cannot fold this layout to the current monitor output "
                              "(only stereo-monitor downmix of speaker / HOA layouts is supported)");
        }
        impl_->engine->switch_stream(
            std::make_unique<realtime::DownmixStream>(std::move(*stream), std::move(*matrix), monitor_channels));
        impl_->current_options = options; // remember the live backend so a device switch rebuilds it
        return {};
    }
    impl_->engine->switch_stream(std::move(*stream));
    impl_->current_options = options; // remember the live backend so a device switch rebuilds it
    return {};
}

Result<void> MonitorSession::set_output_device(const std::string& device_id) {
    if (impl_->engine == nullptr) {
        return make_error(ErrorCode::internal_error, "监听会话无效:后端重建失败");
    }
    if (device_id == impl_->device_id) {
        return {}; // already on this device
    }

    // Snapshot what must survive the device re-open: playhead, play state, and (re-applied)
    // live overrides. The backend is rebuilt from the retained scene + current_options.
    const realtime::MonitorStatus snap = impl_->engine->status();
    const uint64_t playhead = snap.playhead_frames;
    const bool was_playing = snap.state == realtime::MonitorState::playing;

    // Stop the old engine + device first (engine borrows the device by reference), so two
    // devices never pull at once. Then open the new device and rebuild the engine on it.
    impl_->engine.reset();
    impl_->device.reset();

    auto open_on = [&](const std::string& id) -> Result<void> {
        impl_->device = make_monitor_device(impl_->current_options, id);
        auto engine = realtime::MonitorEngine::create(
            *impl_->factory, *impl_->device, impl_->scene, impl_->current_options, impl_->log_sink);
        if (!engine) {
            impl_->device.reset();
            return tl::unexpected{engine.error()};
        }
        impl_->engine = std::move(*engine);
        impl_->device_id = id;
        return {};
    };

    auto opened = open_on(device_id);
    if (!opened) {
        // Fall back to the system default so the session keeps playing rather than dying on a
        // bad device id (e.g. the device was unplugged between enumeration and selection).
        impl_->log_sink.log(LogLevel::warning, "monitor", "切换输出设备失败,回落到默认设备:" + opened.error().message);
        auto fallback = open_on(std::string{});
        if (!fallback) {
            return tl::unexpected{fallback.error()};
        }
    }

    // Restore playhead + edits + play state on the freshly rebuilt engine.
    impl_->engine->seek(playhead);
    impl_->engine->set_overrides(impl_->current_overrides);
    impl_->engine->set_listener_orientation(impl_->effective_orientation()); // 系统空间化→中立(见 setter)
    if (was_playing) {
        impl_->engine->play();
    }
    return {};
}

MonitorStatusSnapshot MonitorSession::status() const {
    if (impl_->engine == nullptr) {
        MonitorStatusSnapshot out; // invalid session → report a stopped/failed snapshot
        out.state = MonitorPlaybackState::stopped;
        out.failed = true;
        return out;
    }
    const realtime::MonitorStatus s = impl_->engine->status();
    MonitorStatusSnapshot out;
    out.state = static_cast<MonitorPlaybackState>(static_cast<std::uint8_t>(s.state));
    out.playhead_frames = s.playhead_frames;
    out.underruns = s.underruns;
    out.buffered_frames = s.buffered_frames;
    out.ring_fill = s.ring_fill;
    out.ended = s.ended;
    out.failed = s.failed;
    out.override_revision = s.override_revision;
    return out;
}

MonitorLevelsSnapshot MonitorSession::levels() const {
    if (impl_->engine == nullptr) {
        return MonitorLevelsSnapshot{}; // invalid session → silent levels
    }
    const realtime::MonitorLevels l = impl_->engine->levels();
    MonitorLevelsSnapshot out;
    out.channels = l.channels;
    out.peak = l.peak;
    out.rms = l.rms;
    out.momentary_lufs = l.momentary_lufs;
    out.shortterm_lufs = l.shortterm_lufs;
    out.integrated_lufs = l.integrated_lufs;
    return out;
}

std::size_t MonitorSession::log_count() const {
    return impl_->log_sink.count();
}

bool MonitorSession::log_entry(std::size_t index, LogLevel& level, std::string& module, std::string& message) const {
    return impl_->log_sink.get(index, level, module, message);
}

} // namespace mradm
