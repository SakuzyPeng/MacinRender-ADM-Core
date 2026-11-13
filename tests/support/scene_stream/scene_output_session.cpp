#include "scene_output_session.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "device_island.h"
#include "ffi_error.h"
#include "routing_ffi.h"

namespace mradm::realtime {
namespace {
using live_scene::FfiError;
struct Destroy {
    void operator()(MradmEngineSceneOutput* handle) const noexcept { mradm_engine_scene_output_destroy(handle); }
};
const MradmEngineSceneStream* stream_handle(const SceneStreamEngine& stream) noexcept {
    return static_cast<const MradmEngineSceneStream*>(stream.rust_handle());
}
MradmAdmText text(const std::string& s) noexcept {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
// Code 100: the device threw; rethrow it as the former shell did.
tl::unexpected<Error> device_error(const FfiError& error, int32_t code) {
    auto e = error.error(code);
    if (code == 100) {
        throw std::runtime_error(e.value().message);
    }
    return e;
}
} // namespace

struct SceneOutputSession::Handle {
    std::unique_ptr<MradmEngineSceneOutput, Destroy> rust;
};

SceneOutputSession::SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream,
                                       std::unique_ptr<Handle> handle,
                                       bool hptf_supported)
    : stream_(std::move(stream)), handle_(std::move(handle)), hptf_supported_(hptf_supported) {}

// The device is opened and driven by Rust mradm-engine `scene::output` (the device islands).
Result<std::unique_ptr<SceneOutputSession>> SceneOutputSession::create(const std::shared_ptr<SceneStreamEngine>& stream,
                                                                       const SceneDeviceConfig& config) {
    if (!stream) {
        return make_error(ErrorCode::invalid_argument, "Scene output requires a stream");
    }
    const auto protect_stereo = config.kind != SceneOutputKind::system_spatial;
    auto handle = std::make_unique<Handle>();
    FfiError error(4096);
    MradmEngineSceneOutput* raw = nullptr;
    if (const auto code = mradm_engine_scene_output_open(stream_handle(*stream),
                                                         static_cast<uint32_t>(config.kind),
                                                         text(config.layout),
                                                         static_cast<uint32_t>(config.geometry),
                                                         text(config.device_id),
                                                         mradm_device_islands(),
                                                         &raw,
                                                         error.message(),
                                                         error.message_len(),
                                                         error.context(),
                                                         error.context_len());
        code != 0) {
        return device_error(error, code);
    }
    handle->rust.reset(raw);
    return std::unique_ptr<SceneOutputSession>{
        new SceneOutputSession(stream, std::move(handle), protect_stereo && stream->output_format().channels == 2U)};
}

Result<std::unique_ptr<SceneOutputSession>> SceneOutputSession::create_with_device(
    const std::shared_ptr<SceneStreamEngine>& stream, std::unique_ptr<IAudioOutputDevice> device, bool protect_stereo) {
    if (!stream || !device) {
        return make_error(ErrorCode::invalid_argument, "Scene output requires a stream and device");
    }
    const auto table = device_table(std::move(device));
    auto handle = std::make_unique<Handle>();
    FfiError error(4096);
    MradmEngineSceneOutput* raw = nullptr;
    if (const auto code = mradm_engine_scene_output_create(stream_handle(*stream),
                                                           &table,
                                                           protect_stereo ? 1U : 0U,
                                                           &raw,
                                                           error.message(),
                                                           error.message_len(),
                                                           error.context(),
                                                           error.context_len());
        code != 0) {
        return device_error(error, code);
    }
    handle->rust.reset(raw);
    return std::unique_ptr<SceneOutputSession>{
        new SceneOutputSession(stream, std::move(handle), protect_stereo && stream->output_format().channels == 2U)};
}

SceneOutputSession::~SceneOutputSession() = default;

void SceneOutputSession::play() {
    mradm_engine_scene_output_play(handle_->rust.get());
}

void SceneOutputSession::pause() {
    mradm_engine_scene_output_pause(handle_->rust.get());
}

Result<void> SceneOutputSession::begin_epoch(std::uint64_t epoch, std::int64_t target) {
    FfiError error;
    if (const auto code = mradm_engine_scene_output_begin_epoch(handle_->rust.get(),
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
    FfiError error;
    if (const auto code = mradm_engine_scene_output_set_volume(
            handle_->rust.get(), gain, error.message(), error.message_len(), error.context(), error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<void> SceneOutputSession::set_hptf(const std::optional<render_common::HptfCoefficients>& coeffs,
                                          std::uint64_t revision) {
    // Rust serialises concurrent setters without parking; the swap is a short blend in the callback.
    std::optional<MradmDspHptfCoefficients> record;
    if (coeffs.has_value()) {
        record = render_common::hptf_record(*coeffs);
    }
    FfiError error;
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
    // Rust designs for the fixed stream rate and serialises publication independently of the output control lock.
    FfiError error;
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
    MradmEngineSceneOutputStatus value{};
    mradm_engine_scene_output_status(handle_->rust.get(), &value);
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
