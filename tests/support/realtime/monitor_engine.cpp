#include "monitor_engine.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

#include "island_stream.h"
#include "render_common.h"
#include "speaker_pcm.h"

namespace mradm::realtime {

namespace {

constexpr int32_t k_construction_failed = 100;

// The source descriptor of a stream: a Rust handle, or an island table that `table` keeps alive
// until the call copies it.
MradmEngineLiveMonitorSource source_of(std::unique_ptr<IRenderStream> stream, MradmEngineIslandStream& table) {
    MradmEngineLiveMonitorSource source{};
    if (auto* raw = render_common::release_engine_stream(stream, source.info); raw != nullptr) {
        source.stream = raw;
        return source;
    }
    table = island_table(std::move(stream));
    source.island = &table;
    return source;
}

} // namespace

void MonitorEngine::Destroy::operator()(MradmEngineLiveMonitor* handle) const noexcept {
    mradm_engine_live_monitor_destroy(handle);
}

MonitorEngine::MonitorEngine(IAudioOutputDevice& device, LogSink& logs) : device_(device), logs_(logs) {
    host_.ctx = this;
    host_.log = [](void* ctx,
                   uint32_t level,
                   const uint8_t* module,
                   size_t module_len,
                   const uint8_t* text,
                   size_t len) noexcept {
        static_cast<MonitorEngine*>(ctx)->logs_.log(static_cast<LogLevel>(level),
                                                    {reinterpret_cast<const char*>(module), module_len},
                                                    {reinterpret_cast<const char*>(text), len});
    };
    host_.device_flush = [](void* ctx) noexcept { static_cast<MonitorEngine*>(ctx)->device_.flush(); };
    host_.device_pause = [](void* ctx) noexcept { static_cast<MonitorEngine*>(ctx)->device_.pause(); };
    host_.device_resume = [](void* ctx) noexcept { static_cast<MonitorEngine*>(ctx)->device_.resume(); };
}

Result<std::unique_ptr<MonitorEngine>>
MonitorEngine::create(IRenderStreamFactory& factory,
                      IAudioOutputDevice& device,
                      const AdmScene& scene,
                      const RenderOptions& opts,
                      LogSink& logs,
                      std::shared_ptr<IRoomControl> (*room_control)(MradmRenderRoomControl*)) {
    auto stream = factory.open(scene, opts, logs);
    if (!stream) {
        return tl::unexpected{stream.error()};
    }
    if (*stream == nullptr) {
        return make_error(ErrorCode::internal_error, "render stream factory returned a null stream");
    }
    std::unique_ptr<MonitorEngine> engine{new MonitorEngine(device, logs)};
    engine->room_control_ = room_control;
    MradmEngineIslandStream table{};
    const auto source = source_of(std::move(*stream), table);
    std::string message(1024, '\0');
    std::string context(1024, '\0');
    auto message_len = message.size();
    auto context_len = context.size();
    MradmEngineLiveMonitor* raw = nullptr;
    if (const auto code = mradm_engine_live_monitor_create(&source,
                                                           device.pull_is_realtime_playback() ? 1U : 0U,
                                                           &engine->host_,
                                                           &raw,
                                                           reinterpret_cast<uint8_t*>(message.data()),
                                                           &message_len,
                                                           reinterpret_cast<uint8_t*>(context.data()),
                                                           &context_len);
        code != 0) {
        message.resize(message_len);
        context.resize(context_len);
        if (code == k_construction_failed) {
            throw std::runtime_error(message);
        }
        return make_error(static_cast<ErrorCode>(code), std::move(message), std::move(context));
    }
    engine->rust_.reset(raw);
    uint32_t output_stage = 0;
    mradm_engine_live_monitor_format(raw, &engine->channels_, &engine->sample_rate_, &output_stage);
    engine->output_stage_ = output_stage != 0U;
    engine->hptf_.borrow(mradm_engine_live_monitor_hptf(raw), engine->channels_, engine->sample_rate_);
    auto started =
        device.start(engine->channels_, engine->sample_rate_, [raw, &device](std::span<float> out, std::size_t frames) {
            MradmEngineLiveMonitorPull result{};
            mradm_engine_live_monitor_pull(raw, out.data(), out.size(), frames, &result);
            if (result.mark_end != 0U) {
                device.mark_end();
            }
            return result.frames;
        });
    if (!started) {
        mradm_engine_live_monitor_begin_shutdown(raw);
        engine->rust_.reset();
        return tl::unexpected{started.error()};
    }
    return engine;
}

MonitorEngine::~MonitorEngine() {
    if (rust_) {
        mradm_engine_live_monitor_begin_shutdown(rust_.get());
    }
    device_.stop(); // stop pulls first so no pull() runs after teardown begins
    rust_.reset();
}

void MonitorEngine::play() {
    mradm_engine_live_monitor_play(rust_.get());
}

void MonitorEngine::pause() {
    mradm_engine_live_monitor_pause(rust_.get());
}

void MonitorEngine::seek(uint64_t frame) {
    mradm_engine_live_monitor_seek(rust_.get(), frame);
}

void MonitorEngine::set_loop(uint64_t start_frame, uint64_t end_frame) {
    mradm_engine_live_monitor_set_loop(rust_.get(), start_frame, end_frame);
}

Result<void> MonitorEngine::set_overrides(const LiveOverrides& overrides) {
    const auto records = render_common::live_override_records(overrides);
    std::string message(512, '\0');
    std::string context(512, '\0');
    for (const auto& item : overrides.objects) {
        context.resize(std::max(context.size(), item.object_id.size() + 1));
    }
    auto message_len = message.size();
    auto context_len = context.size();
    if (const auto code = mradm_engine_live_monitor_overrides(rust_.get(),
                                                              records.data(),
                                                              records.size(),
                                                              overrides.revision,
                                                              reinterpret_cast<uint8_t*>(message.data()),
                                                              &message_len,
                                                              reinterpret_cast<uint8_t*>(context.data()),
                                                              &context_len);
        code != 0) {
        message.resize(message_len);
        context.resize(context_len);
        return make_error(static_cast<ErrorCode>(code), std::move(message), std::move(context));
    }
    return {};
}

void MonitorEngine::set_hptf(const std::optional<render_common::HptfCoefficients>& coeffs, uint64_t revision) {
    if (coeffs.has_value()) {
        hptf_.publish(*coeffs, revision);
    } else {
        hptf_.publish_bypass(revision);
    }
}

void MonitorEngine::set_listener_orientation(const ListenerOrientation& orientation) {
    mradm_engine_live_monitor_orientation(
        rust_.get(), orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg);
}

std::shared_ptr<IRoomControl> MonitorEngine::room_control() const {
    auto* raw = mradm_engine_live_monitor_room_control(rust_.get());
    if (raw == nullptr) {
        return {};
    }
    if (room_control_ == nullptr) {
        mradm_room_control_destroy(raw);
        return {};
    }
    return room_control_(raw);
}

void MonitorEngine::switch_stream(std::unique_ptr<IRenderStream> next, std::shared_ptr<StreamSwitchReceipt> receipt) {
    MradmEngineIslandStream table{};
    const auto source = source_of(std::move(next), table);
    MradmEngineLiveMonitorReceipt record{};
    if (receipt) {
        record.ctx = new std::shared_ptr<StreamSwitchReceipt>(std::move(receipt));
        record.finish = [](void* ctx, uint32_t state) noexcept {
            (*static_cast<std::shared_ptr<StreamSwitchReceipt>*>(ctx))
                ->state.store(state == 1U ? StreamSwitchState::applied : StreamSwitchState::rejected);
        };
        record.release = [](void* ctx) noexcept { delete static_cast<std::shared_ptr<StreamSwitchReceipt>*>(ctx); };
    }
    mradm_engine_live_monitor_switch(rust_.get(), &source, record.ctx != nullptr ? &record : nullptr);
}

MonitorStatus MonitorEngine::status() const {
    MradmEngineLiveMonitorStatus value{};
    mradm_engine_live_monitor_status(rust_.get(), &value);
    MonitorStatus s;
    s.state = static_cast<MonitorState>(value.state);
    s.playhead_frames = value.playhead_frames;
    s.underruns = value.underruns;
    s.buffered_frames = value.buffered_frames;
    s.ring_fill = value.ring_fill;
    s.ended = value.ended != 0U;
    s.failed = value.failed != 0U;
    s.override_revision = value.override_revision;
    return s;
}

MonitorLevels MonitorEngine::levels() const {
    MradmEngineLiveMonitorLevels value{};
    mradm_engine_live_monitor_levels(rust_.get(), &value);
    MonitorLevels l;
    l.channels = value.channels;
    std::copy_n(std::begin(value.peak), k_max_level_channels, l.peak.begin());
    std::copy_n(std::begin(value.rms), k_max_level_channels, l.rms.begin());
    l.momentary_lufs = value.momentary_lufs;
    l.shortterm_lufs = value.shortterm_lufs;
    l.integrated_lufs = value.integrated_lufs;
    return l;
}

} // namespace mradm::realtime
