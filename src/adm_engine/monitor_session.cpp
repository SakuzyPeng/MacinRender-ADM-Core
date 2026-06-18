#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/io.h"
#include "adm/monitor.h"
#include "adm/render.h"

#include "audio_output_device.h"
#include "monitor_engine.h"
#include "render_stream_factory.h"
#include "renderer_factory.h"

namespace mradm {

namespace {

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
        LogLevel level;
        std::string module;
        std::string message;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

// Production stream factory: resolve the backend, build a RenderPlan from the scene +
// options, prepare it, and open a realtime stream. Holds the renderer + prepared state
// alive for the returned stream's lifetime (streams may reference the prepared state).
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
        renderer_ = std::move(resolved->renderer);

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

        auto prepared = renderer_->prepare(plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        prepared_ = std::move(*prepared);
        return renderer_->open_stream(*prepared_, plan, logs);
    }

  private:
    std::string input_path_;
    std::unique_ptr<IRenderer> renderer_;
    std::shared_ptr<IPreparedRender> prepared_;
};

} // namespace

// Declaration order matters for teardown: the engine (which stops the device + joins the
// worker + destroys the stream) is destroyed first, then the device, then the factory whose
// prepared state the stream may reference, then the log buffer.
struct MonitorSession::Impl {
    BufferingLogSink log_sink;
    std::unique_ptr<RealtimeStreamFactory> factory;
    std::unique_ptr<realtime::IAudioOutputDevice> device;
    std::unique_ptr<realtime::MonitorEngine> engine;
    uint32_t sample_rate{48000};
    uint64_t total_frames{0};
};

MonitorSession::MonitorSession() : impl_(std::make_unique<Impl>()) {}
MonitorSession::~MonitorSession() = default;

Result<std::unique_ptr<MonitorSession>> MonitorSession::create(const std::string& input_path,
                                                               const RenderOptions& options) {
    std::unique_ptr<MonitorSession> session{new MonitorSession()};
    Impl& impl = *session->impl_;

    auto scene = io::import_scene(input_path);
    if (!scene) {
        return tl::unexpected{scene.error()};
    }
    for (const auto& warning : scene->import_warnings) {
        impl.log_sink.log(LogLevel::warning, "importer", warning);
    }
    impl.sample_rate = scene->info.sample_rate;
    impl.total_frames = scene->info.num_frames;

    impl.factory = std::make_unique<RealtimeStreamFactory>(input_path);
    impl.device = realtime::make_miniaudio_device(/*use_null_backend=*/false);

    auto engine = realtime::MonitorEngine::create(*impl.factory, *impl.device, *scene, options, impl.log_sink);
    if (!engine) {
        return tl::unexpected{engine.error()};
    }
    impl.engine = std::move(*engine);
    return session;
}

void MonitorSession::play() {
    impl_->engine->play();
}

void MonitorSession::pause() {
    impl_->engine->pause();
}

Result<void> MonitorSession::seek_seconds(double seconds) {
    const double clamped = std::max(0.0, seconds);
    impl_->engine->seek(static_cast<uint64_t>(clamped * impl_->sample_rate));
    return {};
}

void MonitorSession::set_loop_seconds(double start_seconds, double end_seconds) {
    const auto start = static_cast<uint64_t>(std::max(0.0, start_seconds) * impl_->sample_rate);
    const auto end = static_cast<uint64_t>(std::max(0.0, end_seconds) * impl_->sample_rate);
    impl_->engine->set_loop(start, end);
}

MonitorStatusSnapshot MonitorSession::status() const {
    const realtime::MonitorStatus s = impl_->engine->status();
    MonitorStatusSnapshot out;
    out.state = static_cast<MonitorPlaybackState>(static_cast<std::uint8_t>(s.state));
    out.playhead_frames = s.playhead_frames;
    out.underruns = s.underruns;
    out.buffered_frames = s.buffered_frames;
    out.ring_fill = s.ring_fill;
    out.ended = s.ended;
    out.failed = s.failed;
    return out;
}

MonitorLevelsSnapshot MonitorSession::levels() const {
    const realtime::MonitorLevels l = impl_->engine->levels();
    MonitorLevelsSnapshot out;
    out.channels = l.channels;
    out.peak = l.peak;
    out.rms = l.rms;
    return out;
}

std::size_t MonitorSession::log_count() const {
    return impl_->log_sink.count();
}

bool MonitorSession::log_entry(std::size_t index, LogLevel& level, std::string& module, std::string& message) const {
    return impl_->log_sink.get(index, level, module, message);
}

} // namespace mradm
