#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/monitor.h"
#include "adm/render.h"

#ifdef __APPLE__
#include "../adm_apple/apple_island.h"
#endif
#include "../adm_render_room/room_internal.h"
#include "catalog.h"
#include "device_island.h"
#include "hptf_eq.h"
#include "render_common.h"
#include "request_record.h"
#include "scene_handle.h"

// The session logic (log buffer, stream opening, options, edits, HpTF, backend and device
// transactions) is Rust mradm-engine `monitor::session`, which also opens and drives the output
// devices and the Apple islands' streams. This shell registers the islands and imports the scene.
namespace mradm {

namespace {

constexpr int32_t k_thrown = 100;

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

// The scene handle outlives the Rust session, which borrows it (and copies it for an Apple stream).
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct MonitorSession::Impl {
    std::string input_path;
    std::shared_ptr<const detail::RenderSceneHandle> scene_handle;
    MradmEngineMonitorSession* rust{nullptr};

    ~Impl() { mradm_engine_monitor_session_destroy(rust); }
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

namespace {

// Forwards to the session's log buffer (the import's lines).
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

MonitorSession::MonitorSession() : impl_(std::make_unique<Impl>()) {}
MonitorSession::~MonitorSession() = default;

std::vector<AudioOutputDeviceInfo> MonitorSession::list_output_devices() {
    std::vector<AudioOutputDeviceInfo> out;
    const MradmDeviceEnumeration each{
        &out, [](void* user, MradmAdmText id, MradmAdmText name, uint32_t is_default) {
            static_cast<std::vector<AudioOutputDeviceInfo>*>(user)->push_back(
                {render_common::engine_text(id), render_common::engine_text(name), is_default != 0U});
        }};
    mradm_device_islands()->enumerate(&each);
    return out;
}

namespace {

// The islands the session opens: the output devices and, in macOS builds, the Apple backend.
[[nodiscard]] MradmEngineMonitorSessionHost islands() noexcept {
#ifdef __APPLE__
    return {mradm_device_islands(), mradm_apple_islands()};
#else
    return {mradm_device_islands(), nullptr};
#endif
}

} // namespace

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
    impl.scene_handle = std::move(scene->handle);
    const auto info = impl.scene_handle != nullptr ? detail::scene_info(*impl.scene_handle) : std::nullopt;
    if (!info) {
        return make_error(ErrorCode::internal_error, "invalid monitor scene");
    }
    const engine::RenderPlatform platform;
    const auto host = islands();
    impl.rust = mradm_engine_monitor_session_new(
        platform.get(), impl.scene_handle->raw, info->sample_rate, info->frames, &host);
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
    Texts errors(std::accumulate(
        overrides.objects.begin(), overrides.objects.end(), size_t{0}, [](size_t capacity, const auto& item) {
            return std::max(capacity, item.object_id.size() + 1);
        }));
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
