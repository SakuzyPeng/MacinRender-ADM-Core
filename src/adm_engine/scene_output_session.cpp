#include "scene_output_session.h"

#include <utility>

#include "../adm_render_common/routing_ffi.h"
#include "../adm_render_common/rust_error.h"
#include "../adm_render_common/speaker_layouts.h"

namespace mradm::realtime {
namespace {
using live_scene::RustError;
struct Destroy {
    void operator()(MradmEngineSceneOutput* handle) const noexcept { mradm_engine_scene_output_destroy(handle); }
};
const MradmEngineSceneStream* stream_handle(const SceneStreamEngine& stream) noexcept {
    return static_cast<const MradmEngineSceneStream*>(stream.rust_handle());
}
} // namespace

struct SceneOutputSession::Handle {
    std::unique_ptr<MradmEngineSceneOutput, Destroy> rust;
};

SceneOutputSession::SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream,
                                       std::unique_ptr<IAudioOutputDevice> device,
                                       std::unique_ptr<Handle> handle,
                                       bool hptf_supported)
    : stream_(std::move(stream)), device_(std::move(device)), handle_(std::move(handle)),
      hptf_supported_(hptf_supported) {}

Result<std::unique_ptr<SceneOutputSession>> SceneOutputSession::create(const std::shared_ptr<SceneStreamEngine>& stream,
                                                                       const SceneDeviceConfig& config) {
    if (!stream) {
        return make_error(ErrorCode::invalid_argument, "Scene output requires a stream");
    }
    const auto format = stream->output_format();
    std::unique_ptr<IAudioOutputDevice> device;
    if (config.kind == SceneOutputKind::system_spatial) {
        const auto* layout = render_layouts::find_speaker_layout(config.layout, config.geometry);
        if (layout == nullptr || layout->speakers.size() != format.channels) {
            return make_error(ErrorCode::invalid_argument, "Scene output layout does not match the rendered channels");
        }
#ifdef __APPLE__
        device = make_avsamplebuffer_device(config.layout);
#elif defined(_WIN32)
        device = make_spatialaudioclient_device(config.layout, config.geometry);
#else
        return make_error(ErrorCode::unsupported, "System spatial output is unavailable on this platform");
#endif
    } else {
        if (config.kind == SceneOutputKind::stereo && format.channels != 2U) {
            return make_error(ErrorCode::invalid_argument, "Stereo output requires two rendered channels");
        }
        device = make_miniaudio_device(config.kind == SceneOutputKind::null_device, config.device_id);
    }
    return create_with_device(stream, std::move(device), config.kind != SceneOutputKind::system_spatial);
}

Result<std::unique_ptr<SceneOutputSession>> SceneOutputSession::create_with_device(
    const std::shared_ptr<SceneStreamEngine>& stream, std::unique_ptr<IAudioOutputDevice> device, bool protect_stereo) {
    if (!stream || !device) {
        return make_error(ErrorCode::invalid_argument, "Scene output requires a stream and device");
    }
    const auto format = stream->output_format();
    auto handle = std::make_unique<Handle>();
    {
        RustError error;
        MradmEngineSceneOutput* raw = nullptr;
        if (const auto code = mradm_engine_scene_output_create(stream_handle(*stream),
                                                               protect_stereo ? 1U : 0U,
                                                               device->has_device_volume() ? 1U : 0U,
                                                               &raw,
                                                               error.message(),
                                                               error.message_len(),
                                                               error.context(),
                                                               error.context_len());
            code != 0) {
            return error.error(code);
        }
        handle->rust.reset(raw);
    }
    std::unique_ptr<SceneOutputSession> session{
        new SceneOutputSession(stream, std::move(device), std::move(handle), protect_stereo && format.channels == 2U)};
    const auto* rust = session->handle_->rust.get();
    auto* device_ptr = session->device_.get();
    auto started = session->device_->start(
        format.channels, format.sample_rate, [rust, device_ptr](std::span<float> output, std::size_t frames) {
            MradmEngineSceneOutputPull result{};
            mradm_engine_scene_output_pull(rust, output.data(), output.size(), frames, &result);
            if (result.active != 0U) {
                if (result.eos != 0U) {
                    device_ptr->mark_end();
                }
                mradm_engine_scene_output_release(rust);
            }
            return result.frames;
        });
    if (!started) {
        return tl::unexpected(started.error());
    }
    session->device_->pause();
    return session;
}

SceneOutputSession::~SceneOutputSession() {
    mradm_engine_scene_output_park(handle_->rust.get());
    device_->stop();
}

void SceneOutputSession::park() {
    mradm_engine_scene_output_park(handle_->rust.get());
    device_->pause();
}

void SceneOutputSession::play() {
    const std::lock_guard lock(control_);
    mradm_engine_scene_output_play(handle_->rust.get());
    device_->resume();
}

void SceneOutputSession::pause() {
    const std::lock_guard lock(control_);
    park();
}

Result<void> SceneOutputSession::begin_epoch(std::uint64_t epoch, std::int64_t target) {
    const std::lock_guard lock(control_);
    RustError error;
    // Reject a stale epoch before disturbing the current device queue.
    if (const auto code = mradm_engine_scene_output_check_epoch(
            handle_->rust.get(), epoch, error.message(), error.message_len(), error.context(), error.context_len());
        code != 0) {
        return error.error(code);
    }
    park();
    device_->flush();
    if (const auto code = mradm_engine_scene_output_reset_epoch(handle_->rust.get(),
                                                                epoch,
                                                                target,
                                                                error.message(),
                                                                error.message_len(),
                                                                error.context(),
                                                                error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<void> SceneOutputSession::set_volume(float gain) {
    const std::lock_guard lock(control_);
    RustError error;
    if (const auto code = mradm_engine_scene_output_set_volume(
            handle_->rust.get(), gain, error.message(), error.message_len(), error.context(), error.context_len());
        code != 0) {
        return error.error(code);
    }
    device_->set_volume(gain);
    return {};
}

Result<void> SceneOutputSession::set_hptf(const std::optional<render_common::HptfCoefficients>& coeffs,
                                          std::uint64_t revision) {
    // control_ only serialises concurrent setters here — no park(). The publish itself is
    // lock-free, and the swap is a short linear blend inside the callback.
    const std::lock_guard lock(control_);
    std::optional<MradmDspHptfCoefficients> record;
    if (coeffs.has_value()) {
        record = render_common::hptf_record(*coeffs);
    }
    RustError error;
    if (const auto code = mradm_engine_scene_output_set_hptf(handle_->rust.get(),
                                                             record ? &*record : nullptr,
                                                             revision,
                                                             error.message(),
                                                             error.message_len(),
                                                             error.context(),
                                                             error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<void>
SceneOutputSession::set_hptf_profile(const std::string& path, HptfPreampMode mode, std::uint64_t revision) {
    auto settings = render_common::prepare_hptf(path, mode, revision);
    return settings ? set_hptf_prepared(*settings) : tl::unexpected{settings.error()};
}
Result<void>
SceneOutputSession::set_hptf_parameters(const HptfProfile& profile, HptfPreampMode mode, std::uint64_t revision) {
    auto settings = render_common::prepare_hptf(profile, mode, revision);
    return settings ? set_hptf_prepared(*settings) : tl::unexpected{settings.error()};
}
Result<void> SceneOutputSession::set_hptf_prepared(const render_common::HptfSettings& settings) {
    const std::lock_guard lock(control_);
    RustError error;
    if (const auto code = mradm_engine_scene_output_set_hptf_settings(handle_->rust.get(),
                                                                      settings.handle.get(),
                                                                      error.message(),
                                                                      error.message_len(),
                                                                      error.context(),
                                                                      error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}
render_common::HptfSnapshot SceneOutputSession::hptf_snapshot() const {
    MradmDspHptfSnapshot record{};
    mradm_engine_scene_output_hptf_snapshot(handle_->rust.get(), &record);
    return render_common::hptf_snapshot(record);
}
HptfStereoInfo SceneOutputSession::hptf_stereo_info() const {
    return render_common::hptf_snapshot_info(hptf_snapshot());
}
HptfInfo SceneOutputSession::hptf_info() const {
    return hptf_stereo_info().left;
}

SceneDeviceStatus SceneOutputSession::status() const {
    const std::lock_guard lock(control_);
    const auto device = device_->progress();
    const MradmEngineSceneDeviceProgress progress{device.presented_frames,
                                                  device.has_media_clock ? 1U : 0U,
                                                  device.failed ? 1U : 0U,
                                                  device.recovering ? 1U : 0U,
                                                  device.underruns};
    MradmEngineSceneOutputStatus value{};
    mradm_engine_scene_output_status(handle_->rust.get(), &progress, &value);
    SceneDeviceStatus result;
    result.state = static_cast<SceneOutputState>(value.state);
    result.epoch_id = value.epoch;
    result.consumed_frames = value.consumed_frames;
    result.presented_frames = value.presented_frames;
    result.queued_frames = value.queued_frames;
    result.underruns = value.underruns;
    result.media_clock = value.media_clock != 0U;
    result.recovering = value.recovering != 0U;
    result.failed = value.failed != 0U;
    result.ended = value.ended != 0U;
    return result;
}

} // namespace mradm::realtime
