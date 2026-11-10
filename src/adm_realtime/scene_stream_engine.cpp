#include "scene_stream_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "room_internal.h"
#include "scene_renderer_request.h"

namespace mradm::realtime {

namespace {

using live_scene::RustError;

// Policy errors keep the 1024-byte text of the former FFI buffer; the rest are fixed wording.
constexpr std::size_t k_error_text = 1024U;

[[nodiscard]] MradmAdmText text(std::string_view value) noexcept {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

[[nodiscard]] MradmRenderLiveState state_record(const live_scene::ObjectState& s) noexcept {
    return {s.valid_fields,
            s.active ? 1U : 0U,
            s.linear_gain,
            {s.x, s.y, s.z},
            s.width,
            s.height,
            s.depth,
            s.diffuse,
            s.divergence,
            s.channel_lock ? 1U : 0U,
            s.screen_reference ? 1U : 0U,
            s.head_locked ? 1U : 0U,
            s.divergence_azimuth_range,
            s.divergence_position_range,
            s.channel_lock_max_distance ? 1U : 0U,
            s.channel_lock_max_distance.value_or(0.0F)};
}

struct Destroy {
    void operator()(MradmEngineSceneStream* handle) const noexcept { mradm_engine_scene_destroy(handle); }
};

} // namespace

// The epochs, generations, queue, worker, policy, crossfades, timeline, ring, diagnostics and
// `scene/*` checkpoints are Rust mradm-engine `scene::stream` (PCM consistency 5.4 B).
struct SceneStreamEngine::Impl {
    std::unique_ptr<MradmEngineSceneStream, Destroy> handle;
    std::uint32_t output_rate{0U};
    std::uint32_t channels{0U};
    std::uint32_t input_rate{0U};
};

Result<std::unique_ptr<SceneStreamEngine>> SceneStreamEngine::create(const SceneStreamConfig& config) {
    const live_scene::SceneRendererRequest renderer(config.renderer, true);
    const MradmEngineSceneConfig record{config.output_sample_rate,
                                        config.input_queue_samples,
                                        config.input_queue_bytes,
                                        config.output_ring_frames,
                                        config.startup_watermark_frames,
                                        config.manual_clock ? 1U : 0U,
                                        config.manual_clock_ns,
                                        sizeof(live_scene::Frame),
                                        sizeof(live_scene::PcmPlane),
                                        sizeof(live_scene::StateEntry),
                                        sizeof(live_scene::MetadataUpdate)};
    auto error = renderer.error();
    MradmEngineSceneStream* raw = nullptr;
    if (const auto code = mradm_engine_scene_create(
            &record, renderer.get(), &raw, error.message(), error.message_len(), error.context(), error.context_len());
        code != 0) {
        return error.error(code);
    }
    auto impl = std::make_unique<Impl>();
    impl->handle.reset(raw);
    mradm_engine_scene_format(raw, &impl->output_rate, &impl->channels, &impl->input_rate);
    return std::unique_ptr<SceneStreamEngine>(new SceneStreamEngine(std::move(impl)));
}

SceneStreamEngine::SceneStreamEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SceneStreamEngine::~SceneStreamEngine() = default;

SceneOutputFormat SceneStreamEngine::output_format() const noexcept {
    return {impl_->output_rate, impl_->channels};
}

std::uint32_t SceneStreamEngine::input_sample_rate() const noexcept {
    return impl_->input_rate;
}

Result<void> SceneStreamEngine::begin_epoch(std::uint64_t epoch_id, std::int64_t target_sample) {
    RustError error;
    if (const auto code = mradm_engine_scene_begin_epoch(impl_->handle.get(),
                                                         epoch_id,
                                                         target_sample,
                                                         error.message(),
                                                         error.message_len(),
                                                         error.context(),
                                                         error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<void> SceneStreamEngine::configure_generation(std::uint64_t epoch_id,
                                                     std::uint64_t generation_id,
                                                     std::span<const live_scene::ElementDescriptor> elements) {
    std::vector<std::vector<MradmEngineSceneEntity>> contents(elements.size());
    std::vector<std::vector<MradmEngineSceneEntity>> programmes(elements.size());
    std::vector<MradmEngineSceneElement> records(elements.size());
    for (std::size_t index = 0U; index < elements.size(); ++index) {
        const auto& e = elements[index];
        auto& record = records[index];
        record.element_id = e.element_id;
        record.role = static_cast<uint32_t>(e.role);
        record.speaker_label = text(e.speaker_label);
        record.has_position = e.has_position ? 1U : 0U;
        record.position[0] = e.x;
        record.position[1] = e.y;
        record.position[2] = e.z;
        record.flags = e.flags;
        if (!e.semantic_identity) {
            continue;
        }
        const auto& source = *e.semantic_identity;
        for (const auto& content : source.contents) {
            contents[index].push_back({text(content.id), text(content.name)});
        }
        for (const auto& programme : source.programmes) {
            programmes[index].push_back({text(programme.id), text(programme.name)});
        }
        record.identity = {1U,
                           text(source.object_id),
                           text(source.object_name),
                           text(source.track_uid),
                           source.importance ? 1U : 0U,
                           source.importance.value_or(0),
                           source.dialogue_id ? 1U : 0U,
                           source.dialogue_id.value_or(0),
                           contents[index].data(),
                           contents[index].size(),
                           programmes[index].data(),
                           programmes[index].size()};
    }
    RustError error(k_error_text);
    if (const auto code = mradm_engine_scene_configure(impl_->handle.get(),
                                                       epoch_id,
                                                       generation_id,
                                                       records.data(),
                                                       records.size(),
                                                       error.message(),
                                                       error.message_len(),
                                                       error.context(),
                                                       error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<SceneSubmitStatus> SceneStreamEngine::submit_frame(const SceneFrameView& view,
                                                          std::chrono::milliseconds timeout) {
    std::vector<MradmEngineScenePlane> planes(view.pcm.size());
    std::ranges::transform(view.pcm, planes.begin(), [](const auto& plane) -> MradmEngineScenePlane {
        return {plane.element_id, plane.samples, plane.sample_count, plane.stride, plane.has_signal ? 1U : 0U};
    });
    std::vector<MradmRenderLiveEntry> initial(view.initial_states.size());
    std::ranges::transform(view.initial_states, initial.begin(), [](const auto& entry) -> MradmRenderLiveEntry {
        return {entry.element_id, state_record(entry.state)};
    });
    std::vector<MradmRenderLiveUpdate> updates(view.updates.size());
    std::ranges::transform(view.updates, updates.begin(), [](const auto& update) -> MradmRenderLiveUpdate {
        return {update.element_id,
                update.offset_samples,
                update.ramp_duration_samples,
                update.jump_position ? 1U : 0U,
                update.changed_fields,
                update.cleared_fields,
                state_record(update.state)};
    });
    const MradmEngineSceneFrame frame{view.epoch_id,
                                      view.generation_id,
                                      view.media_sample_start,
                                      view.duration_samples,
                                      view.flags,
                                      planes.data(),
                                      planes.size(),
                                      initial.data(),
                                      initial.size(),
                                      updates.data(),
                                      updates.size()};
    RustError error;
    uint32_t submit_status = 0U;
    if (const auto code = mradm_engine_scene_submit(impl_->handle.get(),
                                                    &frame,
                                                    timeout.count(),
                                                    &submit_status,
                                                    error.message(),
                                                    error.message_len(),
                                                    error.context(),
                                                    error.context_len());
        code != 0) {
        return error.error(code);
    }
    return static_cast<SceneSubmitStatus>(submit_status);
}

Result<void> SceneStreamEngine::signal_end(std::uint64_t epoch_id, std::int64_t end_sample) {
    RustError error;
    if (const auto code = mradm_engine_scene_signal_end(impl_->handle.get(),
                                                        epoch_id,
                                                        end_sample,
                                                        error.message(),
                                                        error.message_len(),
                                                        error.context(),
                                                        error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<void> SceneStreamEngine::switch_backend(const live_scene::RendererConfig& config) {
    const live_scene::SceneRendererRequest renderer(config, true);
    auto error = renderer.error();
    if (const auto code = mradm_engine_scene_switch_backend(impl_->handle.get(),
                                                            renderer.get(),
                                                            error.message(),
                                                            error.message_len(),
                                                            error.context(),
                                                            error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

Result<void> SceneStreamEngine::wait_idle(std::chrono::milliseconds timeout) {
    RustError error;
    if (const auto code = mradm_engine_scene_wait_idle(impl_->handle.get(),
                                                       timeout.count(),
                                                       error.message(),
                                                       error.message_len(),
                                                       error.context(),
                                                       error.context_len());
        code != 0) {
        return error.error(code);
    }
    return {};
}

void SceneStreamEngine::set_listener_orientation(const ListenerOrientation& orientation) {
    mradm_engine_scene_orientation(
        impl_->handle.get(), orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg);
}

std::shared_ptr<IRoomControl> SceneStreamEngine::room_control() const {
    return room_internal::control(mradm_engine_scene_room_control(impl_->handle.get()));
}

Result<void> SceneStreamEngine::set_semantic_policy_json(std::string_view json, std::uint64_t revision) {
    RustError error(k_error_text);
    if (const auto code = mradm_engine_scene_policy(impl_->handle.get(),
                                                    reinterpret_cast<const uint8_t*>(json.data()),
                                                    json.size(),
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

ScenePullResult SceneStreamEngine::pull(float* output, std::uint32_t frames) noexcept {
    MradmEngineScenePull result{};
    mradm_engine_scene_pull(
        impl_->handle.get(), output, static_cast<std::size_t>(frames) * impl_->channels, frames, &result);
    return {result.flags, result.epoch, result.first_media_frame, result.media_frames, result.requested_frames};
}

const void* SceneStreamEngine::rust_handle() const noexcept {
    return impl_->handle.get();
}

bool SceneStreamEngine::attach_output() noexcept {
    return mradm_engine_scene_attach(impl_->handle.get(), 1U) != 0U;
}

void SceneStreamEngine::detach_output() noexcept {
    mradm_engine_scene_attach(impl_->handle.get(), 0U);
}

bool SceneStreamEngine::output_attached() const noexcept {
    return mradm_engine_scene_attached(impl_->handle.get()) != 0U;
}

SceneStreamStatus SceneStreamEngine::status() const noexcept {
    MradmEngineSceneStatus s{};
    mradm_engine_scene_status(impl_->handle.get(), &s);
    SceneStreamStatus result;
    result.state = static_cast<SceneStreamState>(s.state);
    result.epoch_id = s.epoch;
    result.generation_id = s.generation;
    result.queued_input_samples = s.queued_input_samples;
    result.queued_input_bytes = s.queued_input_bytes;
    result.buffered_output_frames = s.buffered_output_frames;
    result.media_frames_pulled = s.media_frames_pulled;
    result.underruns = s.underruns;
    result.semantic_degradations = s.semantic_degradations;
    result.semantic_policy_revision = s.semantic_policy_revision;
    result.ring_fill = s.ring_fill;
    result.ended = s.ended != 0U;
    result.failed = s.failed != 0U;
    result.production_complete = s.production_complete != 0U;
    return result;
}

std::size_t SceneStreamEngine::diagnostic_count() const noexcept {
    return mradm_engine_scene_diagnostic_count(impl_->handle.get());
}

std::optional<live_scene::Diagnostic> SceneStreamEngine::diagnostic(std::size_t index) const {
    MradmEngineSceneDiagnostic record{};
    std::string message(256U, '\0');
    const auto read = [&] {
        return mradm_engine_scene_diagnostic(
                   impl_->handle.get(), index, &record, reinterpret_cast<uint8_t*>(message.data()), message.size()) ==
               0;
    };
    if (!read()) {
        return std::nullopt;
    }
    while (record.message_len > message.size()) {
        // A reset or a later error can replace an entry between reads.
        message.resize(record.message_len);
        if (!read()) {
            return std::nullopt;
        }
    }
    message.resize(record.message_len);
    return live_scene::Diagnostic{static_cast<LogLevel>(record.level),
                                  static_cast<live_scene::DiagnosticCode>(record.code),
                                  record.epoch,
                                  record.generation,
                                  record.element,
                                  record.field,
                                  std::move(message)};
}

void SceneStreamEngine::set_clock(std::int64_t ns) noexcept {
    mradm_engine_scene_set_clock(impl_->handle.get(), ns);
}

} // namespace mradm::realtime
