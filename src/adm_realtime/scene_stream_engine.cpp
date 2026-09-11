#include "scene_stream_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <samplerate.h>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/scene.h"
#include "adm/semantic_policy.h"

#include "live_binaural_renderer.h"
#include "live_vbap_renderer.h"
#include "ring_buffer.h"

namespace mradm::realtime {

namespace {

constexpr std::uint32_t k_min_sample_rate = 8000U;
constexpr std::uint32_t k_max_sample_rate = 192000U;
constexpr std::uint32_t k_default_input_samples = 32768U;
constexpr std::uint64_t k_default_input_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t k_default_output_frames = 8192U;
constexpr std::uint32_t k_default_watermark_frames = 4096U;
constexpr std::uint32_t k_resampler_output_frames = 4096U;
constexpr std::uint32_t k_generation_declick_ms = 10U;
constexpr std::uint32_t k_policy_transition_ms = 20U;
constexpr std::uint32_t k_worker_chunk_frames = 1024U;
constexpr std::uint32_t k_tracking_chunk_frames = 512U;
constexpr std::uint32_t k_tracking_lookahead_frames = 2048U;
constexpr std::uint32_t k_backend_crossfade_frames = 2048U;
constexpr auto k_tracking_active_window = std::chrono::milliseconds(750);
constexpr auto k_ring_wait = std::chrono::milliseconds(1);

class DiagnosticStore {
  public:
    void push(live_scene::Diagnostic diagnostic) {
        if (diagnostic.code == live_scene::DiagnosticCode::semantic_degraded ||
            diagnostic.code == live_scene::DiagnosticCode::direct_speaker_fallback ||
            diagnostic.code == live_scene::DiagnosticCode::missing_lfe_output ||
            diagnostic.code == live_scene::DiagnosticCode::incomplete_state) {
            semantic_degradations_.fetch_add(1U, std::memory_order_relaxed);
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(diagnostic));
    }

    [[nodiscard]] std::uint64_t degradation_count() const noexcept {
        return semantic_degradations_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    [[nodiscard]] std::optional<live_scene::Diagnostic> at(std::size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (index >= entries_.size()) {
            return std::nullopt;
        }
        return entries_[index];
    }

  private:
    mutable std::mutex mutex_;
    std::vector<live_scene::Diagnostic> entries_;
    std::atomic<std::uint64_t> semantic_degradations_{0U};
};

struct SrcDeleter {
    void operator()(SRC_STATE* state) const noexcept {
        if (state != nullptr) {
            src_delete(state);
        }
    }
};

using SrcPtr = std::unique_ptr<SRC_STATE, SrcDeleter>;

[[nodiscard]] Result<std::unique_ptr<live_scene::ILiveSceneRenderer>>
create_live_renderer(const live_scene::RendererConfig& config, const live_scene::DiagnosticSink& diagnostics) {
    if (config.renderer == RendererSelection::saf) {
        return live_scene::create_live_vbap_renderer(config, diagnostics);
    }
    if (config.renderer == RendererSelection::saf_binaural) {
        return live_scene::create_live_binaural_renderer(config, diagnostics);
    }
    return make_error(ErrorCode::unsupported, "Scene stream v1 supports only SAF VBAP and SAF binaural renderers");
}

[[nodiscard]] live_scene::ObjectState default_state(const live_scene::ElementDescriptor& descriptor) {
    live_scene::ObjectState state;
    state.valid_fields = live_scene::k_known_state_fields & ~live_scene::state_position;
    if (descriptor.role == live_scene::ElementRole::object) {
        state.valid_fields |= live_scene::state_position;
    }
    if (descriptor.has_position) {
        state.x = descriptor.x;
        state.y = descriptor.y;
        state.z = descriptor.z;
    }
    return state;
}

void copy_state_fields(live_scene::ObjectState& destination,
                       const live_scene::ObjectState& source,
                       std::uint64_t requested_fields) {
    const auto fields = source.valid_fields & requested_fields;
    if ((fields & live_scene::state_active) != 0U) {
        destination.active = source.active;
    }
    if ((fields & live_scene::state_linear_gain) != 0U) {
        destination.linear_gain = source.linear_gain;
    }
    if ((fields & live_scene::state_position) != 0U) {
        destination.x = source.x;
        destination.y = source.y;
        destination.z = source.z;
    }
    if ((fields & live_scene::state_extent) != 0U) {
        destination.width = source.width;
        destination.height = source.height;
        destination.depth = source.depth;
    }
    if ((fields & live_scene::state_diffuse) != 0U) {
        destination.diffuse = source.diffuse;
    }
    if ((fields & live_scene::state_divergence) != 0U) {
        destination.divergence = source.divergence;
    }
    if ((fields & live_scene::state_channel_lock) != 0U) {
        destination.channel_lock = source.channel_lock;
    }
    if ((fields & live_scene::state_screen_reference) != 0U) {
        destination.screen_reference = source.screen_reference;
    }
    if ((fields & live_scene::state_head_locked) != 0U) {
        destination.head_locked = source.head_locked;
    }
    if ((fields & live_scene::state_divergence_range) != 0U) {
        destination.divergence_azimuth_range = source.divergence_azimuth_range;
        destination.divergence_position_range = source.divergence_position_range;
    }
    if ((fields & live_scene::state_channel_lock_max_distance) != 0U) {
        destination.channel_lock_max_distance = source.channel_lock_max_distance;
    }
    destination.valid_fields |= fields;
}

[[nodiscard]] bool object_state_equal(const live_scene::ObjectState& lhs, const live_scene::ObjectState& rhs) noexcept {
    return lhs.valid_fields == rhs.valid_fields && lhs.active == rhs.active && lhs.linear_gain == rhs.linear_gain &&
           lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.width == rhs.width && lhs.height == rhs.height &&
           lhs.depth == rhs.depth && lhs.diffuse == rhs.diffuse && lhs.divergence == rhs.divergence &&
           lhs.channel_lock == rhs.channel_lock && lhs.screen_reference == rhs.screen_reference &&
           lhs.head_locked == rhs.head_locked && lhs.divergence_azimuth_range == rhs.divergence_azimuth_range &&
           lhs.divergence_position_range == rhs.divergence_position_range &&
           lhs.channel_lock_max_distance == rhs.channel_lock_max_distance;
}

[[nodiscard]] bool semantic_entity_equal(const live_scene::SemanticEntity& lhs,
                                         const live_scene::SemanticEntity& rhs) noexcept {
    return lhs.id == rhs.id && lhs.name == rhs.name;
}

[[nodiscard]] bool semantic_identity_equal(const std::optional<live_scene::SemanticIdentity>& lhs,
                                           const std::optional<live_scene::SemanticIdentity>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    return lhs->object_id == rhs->object_id && lhs->object_name == rhs->object_name &&
           lhs->track_uid == rhs->track_uid && lhs->importance == rhs->importance &&
           lhs->dialogue_id == rhs->dialogue_id && lhs->contents.size() == rhs->contents.size() &&
           lhs->programmes.size() == rhs->programmes.size() &&
           std::equal(lhs->contents.begin(), lhs->contents.end(), rhs->contents.begin(), semantic_entity_equal) &&
           std::equal(lhs->programmes.begin(), lhs->programmes.end(), rhs->programmes.begin(), semantic_entity_equal);
}

// Private player-facing completion queue. It knows nothing about devices, clocks,
// epochs, or Scene metadata; the worker is its sole producer and the player's audio
// thread is its sole consumer.
class PlayerOutputEngine {
  public:
    PlayerOutputEngine(std::size_t channels, std::uint32_t capacity_frames)
        : channels_(channels), capacity_frames_(capacity_frames), ring_(channels * capacity_frames) {}

    [[nodiscard]] std::uint32_t capacity_frames() const noexcept { return capacity_frames_; }
    [[nodiscard]] std::size_t buffered_frames() const noexcept { return ring_.available_read() / channels_; }
    [[nodiscard]] std::size_t available_floats() const noexcept { return ring_.available_read(); }
    std::size_t push(const float* source, std::size_t floats) noexcept { return ring_.push(source, floats); }
    std::size_t pop(float* destination, std::size_t floats) noexcept { return ring_.pop(destination, floats); }
    void clear() noexcept { ring_.clear(); }

  private:
    std::size_t channels_{0U};
    std::uint32_t capacity_frames_{0U};
    FloatRingBuffer ring_;
};

[[nodiscard]] bool descriptor_equal(const live_scene::ElementDescriptor& lhs,
                                    const live_scene::ElementDescriptor& rhs) noexcept {
    return lhs.element_id == rhs.element_id && lhs.role == rhs.role && lhs.speaker_label == rhs.speaker_label &&
           lhs.has_position == rhs.has_position && lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z &&
           lhs.flags == rhs.flags && semantic_identity_equal(lhs.semantic_identity, rhs.semantic_identity);
}

[[nodiscard]] bool finite_state(const live_scene::ObjectState& state) noexcept {
    const auto finite = [](float value) { return std::isfinite(value); };
    const auto coordinate = [&](float value) { return finite(value) && value >= -1.0F && value <= 1.0F; };
    if ((state.valid_fields & ~live_scene::k_known_state_fields) != 0U) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_linear_gain) != 0U &&
        (!finite(state.linear_gain) || state.linear_gain < 0.0F)) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_position) != 0U &&
        (!coordinate(state.x) || !coordinate(state.y) || !coordinate(state.z))) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_extent) != 0U &&
        (!finite(state.width) || !finite(state.height) || !finite(state.depth) || state.width < 0.0F ||
         state.width > 1.0F || state.height < 0.0F || state.height > 1.0F || state.depth < 0.0F ||
         state.depth > 1.0F)) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_diffuse) != 0U &&
        (!finite(state.diffuse) || state.diffuse < 0.0F || state.diffuse > 1.0F)) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_divergence) != 0U &&
        (!finite(state.divergence) || state.divergence < 0.0F || state.divergence > 1.0F)) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_divergence_range) != 0U &&
        (!finite(state.divergence_azimuth_range) || state.divergence_azimuth_range < 0.0F ||
         !finite(state.divergence_position_range) || state.divergence_position_range < 0.0F)) {
        return false;
    }
    if ((state.valid_fields & live_scene::state_channel_lock_max_distance) != 0U &&
        state.channel_lock_max_distance.has_value() &&
        (!finite(*state.channel_lock_max_distance) || *state.channel_lock_max_distance < 0.0F)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool add_size(std::size_t& total, std::size_t count, std::size_t item_size) noexcept {
    if (count > (std::numeric_limits<std::size_t>::max() - total) / item_size) {
        return false;
    }
    total += count * item_size;
    return true;
}

void append_unique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::ranges::find(values, value) == values.end()) {
        values.push_back(value);
    }
}

[[nodiscard]] Result<std::vector<SemanticPolicyIdentity>>
// NOLINTNEXTLINE(readability-function-size)
build_policy_identities(std::span<const live_scene::ElementDescriptor> elements) {
    std::vector<SemanticPolicyIdentity> identities(elements.size());
    std::unordered_map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t index = 0U; index < elements.size(); ++index) {
        const auto& optional = elements[index].semantic_identity;
        if (!optional) {
            continue;
        }
        const auto& source = *optional;
        auto& identity = identities[index];
        identity.object_id = source.object_id;
        identity.object_name = source.object_name;
        append_unique(identity.track_uids, source.track_uid);
        identity.importance = source.importance;
        identity.dialogue_id = source.dialogue_id;
        for (const auto& content : source.contents) {
            append_unique(identity.content_ids, content.id);
            append_unique(identity.content_names, content.name);
        }
        for (const auto& programme : source.programmes) {
            append_unique(identity.programme_ids, programme.id);
            append_unique(identity.programme_names, programme.name);
        }
        if (!source.object_id.empty()) {
            groups[source.object_id].push_back(index);
        }
    }

    for (const auto& [object_id, indices] : groups) {
        SemanticPolicyIdentity aggregate;
        aggregate.object_id = object_id;
        for (const auto index : indices) {
            const auto& source = identities[index];
            if (!aggregate.object_name.empty() && !source.object_name.empty() &&
                aggregate.object_name != source.object_name) {
                return make_error(ErrorCode::invalid_argument,
                                  "Scene semantic identities disagree on object_name for object_id " + object_id);
            }
            if (aggregate.importance && source.importance && aggregate.importance != source.importance) {
                return make_error(ErrorCode::invalid_argument,
                                  "Scene semantic identities disagree on importance for object_id " + object_id);
            }
            if (aggregate.dialogue_id && source.dialogue_id && aggregate.dialogue_id != source.dialogue_id) {
                return make_error(ErrorCode::invalid_argument,
                                  "Scene semantic identities disagree on dialogue_id for object_id " + object_id);
            }
            if (aggregate.object_name.empty()) {
                aggregate.object_name = source.object_name;
            }
            if (!aggregate.importance) {
                aggregate.importance = source.importance;
            }
            if (!aggregate.dialogue_id) {
                aggregate.dialogue_id = source.dialogue_id;
            }
            for (const auto& value : source.track_uids) {
                append_unique(aggregate.track_uids, value);
            }
            for (const auto& value : source.content_ids) {
                append_unique(aggregate.content_ids, value);
            }
            for (const auto& value : source.content_names) {
                append_unique(aggregate.content_names, value);
            }
            for (const auto& value : source.programme_ids) {
                append_unique(aggregate.programme_ids, value);
            }
            for (const auto& value : source.programme_names) {
                append_unique(aggregate.programme_names, value);
            }
        }
        for (const auto index : indices) {
            identities[index] = aggregate;
        }
    }
    return identities;
}

} // namespace

// NOLINTBEGIN(misc-non-private-member-variables-in-classes,clang-analyzer-optin.performance.Padding)
struct SceneStreamEngine::Impl {
    struct Generation {
        std::uint64_t id{0U};
        std::vector<live_scene::ElementDescriptor> elements;
        std::vector<SemanticPolicyIdentity> policy_identities;
    };

    struct PendingBackend {
        live_scene::RendererConfig config;
        std::unique_ptr<live_scene::ILiveSceneRenderer> renderer;
    };

    struct PendingControls {
        std::unique_ptr<PendingBackend> backend;
        std::optional<ListenerOrientation> orientation;
        bool has_policy_update{false};
        std::shared_ptr<const SemanticPolicy> policy;
        std::uint64_t policy_revision{0U};

        [[nodiscard]] bool empty() const noexcept {
            return backend == nullptr && !orientation.has_value() && !has_policy_update;
        }
    };

    struct WorkItem {
        enum class Kind : std::uint8_t { frame, end };

        Kind kind{Kind::frame};
        std::uint64_t serial{0U};
        std::shared_ptr<const Generation> generation;
        live_scene::Frame frame;
        std::uint64_t reserved_samples{0U};
        std::uint64_t reserved_bytes{0U};
    };

    Impl(SceneStreamConfig stream_config,
         std::unique_ptr<live_scene::ILiveSceneRenderer> live_renderer,
         std::shared_ptr<DiagnosticStore> diagnostic_store)
        : config(std::move(stream_config)), renderer(std::move(live_renderer)),
          input_sample_rate(config.renderer.sample_rate), diagnostics(std::move(diagnostic_store)),
          channels(renderer->output_channels()), player_output(channels, config.output_ring_frames),
          resample_output(static_cast<std::size_t>(k_resampler_output_frames) * channels, 0.0F),
          zero_output(static_cast<std::size_t>(k_resampler_output_frames) * channels, 0.0F),
          last_output_frame(channels, 0.0F), transition_anchor(channels, 0.0F) {
        if (config.renderer.sample_rate != config.output_sample_rate) {
            int error = 0;
            resampler.reset(src_new(SRC_SINC_MEDIUM_QUALITY, static_cast<int>(channels), &error));
            if (resampler == nullptr) {
                throw std::runtime_error(src_strerror(error));
            }
        }
    }

    ~Impl() {
        reset_gate.store(true, std::memory_order_release);
        quit.store(true, std::memory_order_release);
        queue_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start() {
        worker = std::thread([this] { worker_loop(); });
    }

    [[nodiscard]] bool reset_or_quit(std::uint64_t serial) const noexcept {
        return quit.load(std::memory_order_acquire) || reset_serial.load(std::memory_order_acquire) != serial;
    }

    void add_diagnostic(live_scene::Diagnostic entry) const { diagnostics->push(std::move(entry)); }

    void enter_failed_state() noexcept {
        failed.store(true, std::memory_order_release);
        production_done.store(true, std::memory_order_release);
        output_ready.store(true, std::memory_order_release);
        state.store(SceneStreamState::failed, std::memory_order_release);

        try {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            producer_closed = true;
            queue.clear();
            reserved_samples = 0U;
            reserved_bytes = 0U;
            queued_samples.store(0U, std::memory_order_relaxed);
            queued_bytes.store(0U, std::memory_order_relaxed);
        } catch (...) {
            // The atomic failure state must remain observable even if locking the control queue fails.
            queued_samples.store(0U, std::memory_order_relaxed);
            queued_bytes.store(0U, std::memory_order_relaxed);
            queue_cv.notify_all();
            return;
        }
        queue_cv.notify_all();
    }

    void fail(const Error& error, std::uint64_t epoch_id, std::uint64_t generation_id) noexcept {
        try {
            add_diagnostic({LogLevel::error,
                            live_scene::DiagnosticCode::backend_failure,
                            epoch_id,
                            generation_id,
                            0U,
                            0U,
                            error.message + (error.context.empty() ? std::string{} : ": " + error.context)});
        } catch (...) {
            // Diagnostics are best effort; allocation failure must not keep the stream alive.
            enter_failed_state();
            return;
        }
        enter_failed_state();
    }

    void fail_worker_exception(const char* context, std::uint64_t epoch_id, std::uint64_t generation_id) noexcept {
        try {
            fail(Error{ErrorCode::internal_error, "live Scene worker threw an exception", context},
                 epoch_id,
                 generation_id);
        } catch (...) {
            enter_failed_state();
        }
    }

    void release_budget(const WorkItem& item) {
        const std::lock_guard<std::mutex> lock(queue_mutex);
        if (item.serial != reset_serial.load(std::memory_order_relaxed)) {
            return;
        }
        reserved_samples -= std::min(reserved_samples, item.reserved_samples);
        reserved_bytes -= std::min(reserved_bytes, item.reserved_bytes);
        queued_samples.store(reserved_samples, std::memory_order_relaxed);
        queued_bytes.store(reserved_bytes, std::memory_order_relaxed);
        queue_cv.notify_all();
    }

    [[nodiscard]] bool head_tracking_recent() const noexcept {
        const auto raw = last_orientation_update_ns.load(std::memory_order_relaxed);
        if (raw == 0) {
            return false;
        }
        const std::chrono::steady_clock::time_point last{std::chrono::steady_clock::duration{raw}};
        return (std::chrono::steady_clock::now() - last) < k_tracking_active_window;
    }

    [[nodiscard]] std::size_t output_lookahead_frames() const noexcept {
        const auto capacity = static_cast<std::size_t>(player_output.capacity_frames());
        return head_tracking_recent() ? std::min<std::size_t>(capacity, k_tracking_lookahead_frames) : capacity;
    }

    [[nodiscard]] std::size_t effective_startup_watermark() const noexcept {
        return std::min<std::size_t>(config.startup_watermark_frames, output_lookahead_frames());
    }

    void publish_output_readiness(std::size_t buffered, std::size_t lookahead) noexcept {
        if (buffered >= std::min<std::size_t>(config.startup_watermark_frames, lookahead)) {
            output_ready.store(true, std::memory_order_release);
            state.store(SceneStreamState::running, std::memory_order_release);
        }
    }

    void report_backend_switch_failure(std::string detail) const {
        add_diagnostic({LogLevel::error,
                        live_scene::DiagnosticCode::backend_failure,
                        epoch_status.load(std::memory_order_relaxed),
                        current_generation,
                        0U,
                        0U,
                        "live Scene backend switch was rejected: " + std::move(detail)});
    }

    void begin_backend_switch(std::unique_ptr<PendingBackend> backend) {
        if (!backend) {
            return;
        }
        try {
            backend->renderer->set_listener_orientation(current_orientation);
            if (has_current_generation && current_generation_model) {
                auto configured =
                    backend->renderer->configure_generation(current_generation, current_generation_model->elements);
                if (!configured) {
                    report_backend_switch_failure(configured.error().message);
                    return;
                }
            }
        } catch (const std::exception& exception) {
            report_backend_switch_failure(std::string{"candidate backend threw while being configured: "} +
                                          exception.what());
            return;
        } catch (...) {
            report_backend_switch_failure("candidate backend threw while being configured");
            return;
        }

        if (!has_current_generation || !current_generation_model) {
            renderer = std::move(backend->renderer);
            config.renderer = std::move(backend->config);
            return;
        }
        incoming_config = std::move(backend->config);
        incoming_renderer = std::move(backend->renderer);
        backend_crossfade_position = 0U;
        incoming_needs_initial_state = true;
    }

    void start_deferred_backend_switch() {
        if (incoming_renderer || !deferred_backend) {
            return;
        }
        auto next = std::move(deferred_backend);
        begin_backend_switch(std::move(next));
    }

    void abandon_incoming_backend() {
        incoming_renderer = nullptr;
        backend_crossfade_position = 0U;
        incoming_needs_initial_state = false;
        start_deferred_backend_switch();
    }

    void finalize_backend_switch() {
        if (!incoming_renderer) {
            return;
        }
        renderer = std::move(incoming_renderer);
        config.renderer = std::move(incoming_config);
        backend_crossfade_position = 0U;
        incoming_needs_initial_state = false;
        start_deferred_backend_switch();
    }

    [[nodiscard]] std::vector<ResolvedSemanticPolicy>
    resolve_policy_for_current_generation(const std::shared_ptr<const SemanticPolicy>& policy) {
        if (!current_generation_model) {
            return {};
        }
        std::vector<ResolvedSemanticPolicy> resolved(current_generation_model->elements.size());
        if (!policy) {
            return resolved;
        }
        std::unordered_set<std::size_t> matched;
        for (std::size_t index = 0U; index < current_generation_model->policy_identities.size(); ++index) {
            resolved[index] = resolve_semantic_policy(*policy, current_generation_model->policy_identities[index]);
            matched.insert(resolved[index].matched_rule_indices.begin(), resolved[index].matched_rule_indices.end());
        }
        for (std::size_t index = 0U; index < policy->objects.size(); ++index) {
            if (matched.contains(index)) {
                continue;
            }
            add_diagnostic({LogLevel::warning,
                            live_scene::DiagnosticCode::semantic_degraded,
                            epoch_status.load(std::memory_order_relaxed),
                            current_generation,
                            0U,
                            0U,
                            fmt::format("semantic policy objects[{}] did not match this Scene generation", index)});
        }
        return resolved;
    }

    void compile_policy_for_current_generation() {
        resolved_policy = resolve_policy_for_current_generation(current_policy);
    }

    // NOLINTNEXTLINE(readability-function-size)
    [[nodiscard]] live_scene::ObjectState apply_policy_to_state(std::size_t element_index,
                                                                const live_scene::ObjectState& base,
                                                                live_scene::MetadataUpdate* update = nullptr) const {
        if (!current_policy || element_index >= resolved_policy.size() || !current_generation_model) {
            return base;
        }
        const auto& descriptor = current_generation_model->elements[element_index];
        const auto& resolved = resolved_policy[element_index];
        live_scene::ObjectState out = base;

        SceneObject object;
        object.gain = 1.0F;
        apply_resolved_semantic_object(object, resolved.object);
        const float object_gain = object.mute ? 0.0F : object.gain;

        if (descriptor.role == live_scene::ElementRole::object) {
            SceneObjectBlock block;
            block.position.cartesian = true;
            block.position.x = base.x;
            block.position.y = base.y;
            block.position.z = base.z;
            block.gain = base.linear_gain;
            block.diffuse = base.diffuse;
            block.width = base.width;
            block.height = base.height;
            block.depth = base.depth;
            block.channel_lock = base.channel_lock;
            block.channel_lock_max_distance = base.channel_lock_max_distance;
            block.divergence = base.divergence;
            block.divergence_azimuth_range = base.divergence_azimuth_range;
            block.divergence_position_range = base.divergence_position_range;
            block.screen_ref = base.screen_reference;
            block.head_locked = base.head_locked;
            if (update != nullptr) {
                block.jump_position = update->jump_position;
                if (update->ramp_duration_samples != 0U) {
                    block.interp_length_samples = update->ramp_duration_samples;
                }
            }
            apply_resolved_semantic_object_block(block, resolved.object, config.renderer.sample_rate);
            out.linear_gain = block.gain * object_gain;
            out.diffuse = block.diffuse;
            out.width = block.width;
            out.height = block.height;
            out.depth = block.depth;
            out.channel_lock = block.channel_lock;
            out.channel_lock_max_distance = block.channel_lock_max_distance;
            out.divergence = block.divergence;
            out.divergence_azimuth_range = block.divergence_azimuth_range;
            out.divergence_position_range = block.divergence_position_range;
            out.screen_reference = block.screen_ref;
            out.head_locked = block.head_locked;
            if (!block.position.cartesian) {
                const auto direction = direction_vector_from_polar(block.position.azimuth, block.position.elevation);
                out.x = direction.x * block.position.distance;
                out.y = direction.y * block.position.distance;
                out.z = direction.z * block.position.distance;
            } else {
                out.x = block.position.x;
                out.y = block.position.y;
                out.z = block.position.z;
            }
            if (update != nullptr) {
                update->jump_position = block.jump_position;
                update->ramp_duration_samples =
                    block.interp_length_samples.has_value()
                        ? static_cast<std::uint32_t>(std::min<std::uint64_t>(*block.interp_length_samples,
                                                                             std::numeric_limits<std::uint32_t>::max()))
                        : 0U;
            }
            return out;
        }

        SceneDirectSpeakersBlock block;
        if (!descriptor.speaker_label.empty()) {
            block.speaker_labels.push_back(descriptor.speaker_label);
        }
        if (descriptor.role == live_scene::ElementRole::lfe) {
            block.low_pass_hz = 120.0F;
        }
        block.gain = base.linear_gain;
        block.head_locked = base.head_locked;
        const bool producer_has_position = (base.valid_fields & live_scene::state_position) != 0U;
        if (producer_has_position) {
            const auto polar =
                scene_position_to_polar(SceneBlockPosition{true, 0.0F, 0.0F, 1.0F, base.x, base.y, base.z});
            block.azimuth = polar.azimuth;
            block.elevation = polar.elevation;
            block.distance = polar.distance;
            block.has_position = true;
        } else if (descriptor.has_position) {
            const auto polar = scene_position_to_polar(
                SceneBlockPosition{true, 0.0F, 0.0F, 1.0F, descriptor.x, descriptor.y, descriptor.z});
            block.azimuth = polar.azimuth;
            block.elevation = polar.elevation;
            block.distance = polar.distance;
            block.has_position = true;
        }
        // A descriptor position is only a routing fallback. Probe the resolved
        // DirectSpeakers rules separately so only a matching position operation
        // promotes that fallback into the renderer's explicit state.
        auto position_probe = block;
        position_probe.has_position = false;
        apply_resolved_semantic_direct_speaker(position_probe, resolved.direct_speakers, resolved.object);
        const bool policy_has_position = position_probe.has_position;
        apply_resolved_semantic_direct_speaker(block, resolved.direct_speakers, resolved.object);
        out.linear_gain = block.gain * object_gain;
        out.head_locked = block.head_locked;
        if (block.has_position && (producer_has_position || policy_has_position)) {
            const auto direction = direction_vector_from_polar(block.azimuth, block.elevation);
            out.x = direction.x * block.distance;
            out.y = direction.y * block.distance;
            out.z = direction.z * block.distance;
            out.valid_fields |= live_scene::state_position;
        } else {
            out.valid_fields &= ~live_scene::state_position;
        }
        return out;
    }

    [[nodiscard]] std::vector<live_scene::StateEntry> effective_state_snapshot() const {
        std::vector<live_scene::StateEntry> snapshot;
        if (!current_generation_model) {
            return snapshot;
        }
        snapshot.reserve(current_generation_model->elements.size());
        for (const auto& descriptor : current_generation_model->elements) {
            const auto found = effective_states.find(descriptor.element_id);
            snapshot.push_back(
                {descriptor.element_id, found != effective_states.end() ? found->second : default_state(descriptor)});
        }
        return snapshot;
    }

    [[nodiscard]] PendingControls take_pending_controls() {
        PendingControls controls;
        const std::lock_guard<std::mutex> lock(queue_mutex);
        if (reset_request != 0U) {
            return controls;
        }
        controls.backend = std::move(pending_backend);
        if (orientation_pending) {
            controls.orientation = pending_orientation;
            orientation_pending = false;
        }
        if (policy_pending) {
            controls.has_policy_update = true;
            controls.policy = std::move(pending_policy);
            controls.policy_revision = pending_policy_revision;
            policy_pending = false;
        }
        return controls;
    }

    void apply_pending_controls(std::unique_ptr<PendingBackend> backend,
                                std::optional<ListenerOrientation> orientation,
                                bool has_policy_update,
                                std::shared_ptr<const SemanticPolicy> policy,
                                std::uint64_t policy_revision) {
        if (has_policy_update) {
            try {
                auto next_resolved = resolve_policy_for_current_generation(policy);
                current_policy = std::move(policy);
                resolved_policy = std::move(next_resolved);
                policy_retarget_pending = has_current_generation;
                applied_policy_revision.store(policy_revision, std::memory_order_release);
            } catch (const std::exception& exception) {
                add_diagnostic({LogLevel::error,
                                live_scene::DiagnosticCode::semantic_degraded,
                                epoch_status.load(std::memory_order_relaxed),
                                current_generation,
                                0U,
                                0U,
                                std::string{"live Scene semantic policy preparation failed; keeping the previous "
                                            "policy: "} +
                                    exception.what()});
            } catch (...) {
                add_diagnostic({LogLevel::error,
                                live_scene::DiagnosticCode::semantic_degraded,
                                epoch_status.load(std::memory_order_relaxed),
                                current_generation,
                                0U,
                                0U,
                                "live Scene semantic policy preparation failed; keeping the previous policy"});
            }
        }
        if (orientation) {
            current_orientation = *orientation;
            renderer->set_listener_orientation(current_orientation);
            if (incoming_renderer) {
                incoming_renderer->set_listener_orientation(current_orientation);
            }
            if (deferred_backend) {
                deferred_backend->renderer->set_listener_orientation(current_orientation);
            }
        }
        if (!backend) {
            return;
        }
        if (incoming_renderer && backend_crossfade_position > 0U) {
            // Replacing an already-audible incoming renderer would jump back to
            // the outgoing backend. Finish this curve, then start the latest one.
            deferred_backend = std::move(backend);
            return;
        }
        incoming_renderer = nullptr;
        backend_crossfade_position = 0U;
        incoming_needs_initial_state = false;
        deferred_backend.reset();
        begin_backend_switch(std::move(backend));
    }

    void apply_available_controls() {
        auto controls = take_pending_controls();
        if (controls.empty()) {
            return;
        }
        apply_pending_controls(std::move(controls.backend),
                               controls.orientation,
                               controls.has_policy_update,
                               std::move(controls.policy),
                               controls.policy_revision);
    }

    void perform_reset(std::uint64_t serial, std::int64_t target_sample) {
        std::unique_ptr<PendingBackend> latest_backend;
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            latest_backend = std::move(pending_backend);
        }
        if (!latest_backend) {
            latest_backend = std::move(deferred_backend);
        }
        if (latest_backend) {
            renderer = std::move(latest_backend->renderer);
            config.renderer = std::move(latest_backend->config);
            incoming_renderer = nullptr;
        } else if (incoming_renderer) {
            renderer = std::move(incoming_renderer);
            config.renderer = std::move(incoming_config);
        }
        deferred_backend.reset();
        backend_crossfade_position = 0U;
        incoming_needs_initial_state = false;
        (*renderer).reset();
        renderer->set_listener_orientation(current_orientation);
        if (resampler != nullptr) {
            src_reset(resampler.get());
        }
        player_output.clear();
        pending_output.clear();
        pending_frame_offset = 0U;
        current_generation = 0U;
        has_current_generation = false;
        current_generation_model.reset();
        current_element_index.clear();
        resolved_policy.clear();
        base_states.clear();
        effective_states.clear();
        policy_retarget_pending = false;
        target_sample_worker = target_sample;
        preroll_output_boundary = 0U;
        preroll_rational_remainder = 0U;
        preroll_output_frames_generated = 0U;
        output_frames_to_skip = 0U;
        target_reached = false;
        rational_remainder = 0U;
        allowed_output_frames = 0U;
        output_frames_pushed = 0U;
        transition_remaining = 0U;
        transition_position = 0U;
        std::ranges::fill(last_output_frame, 0.0F);
        std::ranges::fill(transition_anchor, 0.0F);
        media_frames_pulled.store(0U, std::memory_order_relaxed);
        underruns.store(0U, std::memory_order_relaxed);
        output_ready.store(false, std::memory_order_relaxed);
        production_done.store(false, std::memory_order_relaxed);
        failed.store(false, std::memory_order_relaxed);
        generation_status.store(0U, std::memory_order_relaxed);
        state.store(SceneStreamState::buffering, std::memory_order_release);

        const std::lock_guard<std::mutex> lock(queue_mutex);
        reset_ack = serial;
        queue_cv.notify_all();
    }

    void acknowledge_failed_reset(std::uint64_t serial) noexcept {
        try {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            reset_ack = std::max(reset_ack, serial);
        } catch (...) {
            // A broken control mutex cannot support recovery; wake the waiter through the terminal predicate.
            quit.store(true, std::memory_order_release);
        }
        queue_cv.notify_all();
    }

    void worker_loop() {
        while (!quit.load(std::memory_order_acquire)) {
            WorkItem item;
            bool have_item = false;
            PendingControls controls;
            std::uint64_t pending_reset = 0U;
            std::int64_t pending_target = 0;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] {
                    return quit.load(std::memory_order_acquire) || reset_request != 0U || !queue.empty() ||
                           pending_backend != nullptr || orientation_pending || policy_pending;
                });
                if (quit.load(std::memory_order_acquire)) {
                    return;
                }
                if (reset_request != 0U) {
                    pending_reset = reset_request;
                    pending_target = reset_target;
                    reset_request = 0U;
                } else {
                    controls.backend = std::move(pending_backend);
                    if (orientation_pending) {
                        controls.orientation = pending_orientation;
                        orientation_pending = false;
                    }
                    if (policy_pending) {
                        controls.has_policy_update = true;
                        controls.policy = std::move(pending_policy);
                        controls.policy_revision = pending_policy_revision;
                        policy_pending = false;
                    }
                    if (!queue.empty()) {
                        item = std::move(queue.front());
                        queue.pop_front();
                        have_item = true;
                    }
                }
            }
            const auto epoch_id = item.frame.epoch_id != 0U ? item.frame.epoch_id : epoch_status.load();
            try {
                if (pending_reset != 0U) {
                    perform_reset(pending_reset, pending_target);
                    continue;
                }
                if (!controls.empty()) {
                    apply_pending_controls(std::move(controls.backend),
                                           controls.orientation,
                                           controls.has_policy_update,
                                           std::move(controls.policy),
                                           controls.policy_revision);
                }
                if (!have_item || item.serial != reset_serial.load(std::memory_order_acquire)) {
                    continue;
                }

                Result<void> processed;
                if (item.kind == WorkItem::Kind::frame) {
                    processed = process_frame(item);
                } else {
                    processed = process_end(item);
                }
                release_budget(item);
                if (!processed && !reset_or_quit(item.serial)) {
                    fail(processed.error(), epoch_id, item.frame.generation_id);
                }
            } catch (const std::exception& exception) {
                if (pending_reset != 0U) {
                    acknowledge_failed_reset(pending_reset);
                }
                if (!have_item || !reset_or_quit(item.serial)) {
                    fail_worker_exception(exception.what(), epoch_id, item.frame.generation_id);
                }
            } catch (...) {
                if (pending_reset != 0U) {
                    acknowledge_failed_reset(pending_reset);
                }
                if (!have_item || !reset_or_quit(item.serial)) {
                    fail_worker_exception("unknown exception", epoch_id, item.frame.generation_id);
                }
            }
        }
    }

    [[nodiscard]] Result<bool> switch_generation(const WorkItem& item) {
        if (item.generation == nullptr) {
            return make_error(ErrorCode::internal_error, "Scene frame lost its generation descriptor");
        }
        if (has_current_generation && current_generation == item.generation->id) {
            return false;
        }
        auto configured = renderer->configure_generation(item.generation->id, item.generation->elements);
        if (!configured) {
            return tl::unexpected{configured.error()};
        }
        if (incoming_renderer) {
            std::optional<std::string> incoming_failure;
            try {
                auto incoming_configured =
                    incoming_renderer->configure_generation(item.generation->id, item.generation->elements);
                if (!incoming_configured) {
                    incoming_failure = incoming_configured.error().message;
                }
            } catch (const std::exception& exception) {
                incoming_failure =
                    std::string{"candidate backend threw while configuring a generation: "} + exception.what();
            } catch (...) {
                incoming_failure = "candidate backend threw while configuring a generation";
            }
            if (incoming_failure) {
                add_diagnostic({LogLevel::error,
                                live_scene::DiagnosticCode::backend_failure,
                                item.frame.epoch_id,
                                item.generation->id,
                                0U,
                                0U,
                                "incoming live Scene backend rejected a generation; keeping the current backend: " +
                                    *incoming_failure});
                incoming_renderer = nullptr;
                backend_crossfade_position = 0U;
                incoming_needs_initial_state = false;
            }
        }
        current_generation = item.generation->id;
        has_current_generation = true;
        current_generation_model = item.generation;
        current_element_index.clear();
        base_states.clear();
        effective_states.clear();
        current_element_index.reserve(item.generation->elements.size());
        base_states.reserve(item.generation->elements.size());
        effective_states.reserve(item.generation->elements.size());
        for (std::size_t index = 0U; index < item.generation->elements.size(); ++index) {
            const auto& descriptor = item.generation->elements[index];
            current_element_index.emplace(descriptor.element_id, index);
            base_states.emplace(descriptor.element_id, default_state(descriptor));
        }
        try {
            compile_policy_for_current_generation();
        } catch (const std::exception& exception) {
            resolved_policy.assign(item.generation->elements.size(), {});
            add_diagnostic({LogLevel::error,
                            live_scene::DiagnosticCode::semantic_degraded,
                            item.frame.epoch_id,
                            item.generation->id,
                            0U,
                            0U,
                            std::string{"live Scene semantic policy could not be prepared for the new generation; "
                                        "using producer state: "} +
                                exception.what()});
        } catch (...) {
            resolved_policy.assign(item.generation->elements.size(), {});
            add_diagnostic({LogLevel::error,
                            live_scene::DiagnosticCode::semantic_degraded,
                            item.frame.epoch_id,
                            item.generation->id,
                            0U,
                            0U,
                            "live Scene semantic policy could not be prepared for the new generation; using "
                            "producer state"});
        }
        for (std::size_t index = 0U; index < item.generation->elements.size(); ++index) {
            const auto& descriptor = item.generation->elements[index];
            effective_states.emplace(descriptor.element_id,
                                     apply_policy_to_state(index, base_states.at(descriptor.element_id)));
        }
        policy_retarget_pending = false;
        start_deferred_backend_switch();
        incoming_needs_initial_state = incoming_renderer != nullptr;
        generation_status.store(current_generation, std::memory_order_release);
        transition_anchor = last_output_frame;
        transition_remaining = std::max<std::uint64_t>(
            1U, (static_cast<std::uint64_t>(config.output_sample_rate) * k_generation_declick_ms) / 1000U);
        transition_position = 0U;
        return true;
    }

    void apply_initial_states(std::span<const live_scene::StateEntry> source,
                              bool generation_switched,
                              std::vector<live_scene::StateEntry>& converted) {
        for (const auto& initial : source) {
            const auto found = current_element_index.find(initial.element_id);
            if (found == current_element_index.end()) {
                continue;
            }
            const auto& descriptor = current_generation_model->elements[found->second];
            auto base = default_state(descriptor);
            copy_state_fields(base, initial.state, initial.state.valid_fields);
            base_states[initial.element_id] = base;
            effective_states[initial.element_id] = apply_policy_to_state(found->second, base);
        }

        if (generation_switched) {
            converted = effective_state_snapshot();
            return;
        }
        converted.reserve(source.size());
        for (const auto& initial : source) {
            const auto found = effective_states.find(initial.element_id);
            if (found != effective_states.end()) {
                converted.push_back({initial.element_id, found->second});
            }
        }
    }

    void append_policy_retargets(std::vector<live_scene::MetadataUpdate>& updates, std::uint64_t& stream_order) {
        if (!policy_retarget_pending || !current_generation_model) {
            return;
        }
        const auto transition_samples = std::max<std::uint32_t>(
            1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(config.renderer.sample_rate) * k_policy_transition_ms) / 1000U));
        for (std::size_t index = 0U; index < current_generation_model->elements.size(); ++index) {
            const auto& descriptor = current_generation_model->elements[index];
            const auto base = base_states.find(descriptor.element_id);
            const auto producer_state = base != base_states.end() ? base->second : default_state(descriptor);
            const auto next = apply_policy_to_state(index, producer_state);
            const auto previous = effective_states.find(descriptor.element_id);
            if (previous == effective_states.end() || !object_state_equal(previous->second, next)) {
                live_scene::MetadataUpdate update;
                update.element_id = descriptor.element_id;
                update.offset_samples = 0U;
                update.ramp_duration_samples = transition_samples;
                update.jump_position = false;
                update.changed_fields = next.valid_fields;
                update.state = next;
                update.stream_order = stream_order++;
                if (previous != effective_states.end()) {
                    update.cleared_fields = previous->second.valid_fields & ~next.valid_fields;
                }
                updates.push_back(update);
            }
            effective_states[descriptor.element_id] = next;
        }
        policy_retarget_pending = false;
    }

    void append_producer_update(const live_scene::MetadataUpdate& source,
                                std::uint32_t slice_start,
                                std::vector<live_scene::MetadataUpdate>& updates,
                                std::uint64_t& stream_order) {
        const auto found = current_element_index.find(source.element_id);
        if (found == current_element_index.end()) {
            return;
        }
        const auto& descriptor = current_generation_model->elements[found->second];
        auto base =
            base_states.contains(source.element_id) ? base_states.at(source.element_id) : default_state(descriptor);
        const auto previous_effective = apply_policy_to_state(found->second, base);
        copy_state_fields(base, source.state, source.changed_fields);
        base_states[source.element_id] = base;

        auto converted = source;
        converted.offset_samples -= slice_start;
        converted.state = apply_policy_to_state(found->second, base, &converted);
        // Retarget explicit producer fields plus any fields coupled by semantic
        // policy. Unchanged fields retain their own in-flight ramp and deadline.
        converted.changed_fields = source.changed_fields & converted.state.valid_fields;
        for (std::uint64_t field = 1U; field <= live_scene::state_channel_lock_max_distance; field <<= 1U) {
            auto probe = previous_effective;
            copy_state_fields(probe, converted.state, field);
            if (!object_state_equal(probe, previous_effective)) {
                converted.changed_fields |= field;
            }
        }
        converted.stream_order = stream_order++;
        effective_states[source.element_id] = converted.state;
        updates.push_back(converted);
    }

    [[nodiscard]] Result<void> render_slice(const live_scene::Frame& frame,
                                            const std::vector<live_scene::StateEntry>& incoming_snapshot) {
        render_output.resize(static_cast<std::size_t>(frame.duration_samples) * channels);
        auto rendered = renderer->render(frame, render_output);
        if (!rendered) {
            return tl::unexpected{rendered.error()};
        }
        if (!incoming_renderer) {
            return {};
        }

        live_scene::Frame incoming_frame = frame;
        if (incoming_needs_initial_state) {
            incoming_frame.initial_states = incoming_snapshot;
        }
        render_output_b.resize(static_cast<std::size_t>(frame.duration_samples) * channels);
        auto incoming_rendered = incoming_renderer->render(incoming_frame, render_output_b);
        if (!incoming_rendered) {
            add_diagnostic({LogLevel::error,
                            live_scene::DiagnosticCode::backend_failure,
                            frame.epoch_id,
                            frame.generation_id,
                            0U,
                            0U,
                            "incoming live Scene backend failed while crossfading; keeping the current backend: " +
                                incoming_rendered.error().message});
            abandon_incoming_backend();
            return {};
        }
        incoming_needs_initial_state = false;

        for (std::uint32_t frame_index = 0U; frame_index < frame.duration_samples; ++frame_index) {
            const auto position = std::min<std::uint64_t>(backend_crossfade_position + 1U, k_backend_crossfade_frames);
            const float incoming_weight = static_cast<float>(position) / static_cast<float>(k_backend_crossfade_frames);
            const float outgoing_weight = 1.0F - incoming_weight;
            const auto output_index = static_cast<std::size_t>(frame_index) * channels;
            for (std::size_t channel = 0U; channel < channels; ++channel) {
                render_output[output_index + channel] = (render_output[output_index + channel] * outgoing_weight) +
                                                        (render_output_b[output_index + channel] * incoming_weight);
            }
            ++backend_crossfade_position;
        }
        if (backend_crossfade_position >= k_backend_crossfade_frames) {
            finalize_backend_switch();
        }
        return {};
    }

    [[nodiscard]] Result<void> feed_rendered_slice(const live_scene::Frame& frame, std::uint64_t serial) {
        const auto frame_end = frame.media_sample_start + static_cast<std::int64_t>(frame.duration_samples);
        std::uint32_t hidden_frames = 0U;
        if (frame.media_sample_start < target_sample_worker) {
            const auto hidden_end = std::min(frame_end, target_sample_worker);
            hidden_frames = static_cast<std::uint32_t>(hidden_end - frame.media_sample_start);
        }
        if (hidden_frames > 0U) {
            add_preroll_samples(hidden_frames);
            auto hidden = feed_samples(render_output.data(), hidden_frames, false, false, false, true, serial);
            if (!hidden) {
                return tl::unexpected{hidden.error()};
            }
        }
        if (hidden_frames == frame.duration_samples) {
            return {};
        }

        finish_preroll();
        const auto audible_frames = frame.duration_samples - hidden_frames;
        const bool state_complete = (frame.flags & live_scene::frame_state_complete) != 0U;
        const bool warmup = (frame.flags & live_scene::frame_warmup) != 0U;
        float* audible = render_output.data() + (static_cast<std::size_t>(hidden_frames) * channels);
        const bool force_silence = !state_complete || warmup;
        if (force_silence) {
            std::fill_n(audible, static_cast<std::size_t>(audible_frames) * channels, 0.0F);
        }
        return feed_samples(audible, audible_frames, true, true, force_silence, false, serial);
    }

    [[nodiscard]] Result<void> process_frame(const WorkItem& item) {
        if (reset_or_quit(item.serial)) {
            return {};
        }
        auto switched = switch_generation(item);
        if (!switched) {
            return tl::unexpected{switched.error()};
        }

        const auto& frame = item.frame;
        std::size_t update_index = 0U;
        std::uint32_t slice_start = 0U;
        bool first_slice = true;
        while (slice_start < frame.duration_samples) {
            if (reset_or_quit(item.serial)) {
                return {};
            }
            apply_available_controls();
            if (reset_or_quit(item.serial)) {
                return {};
            }
            const auto maximum = head_tracking_recent() ? k_tracking_chunk_frames : k_worker_chunk_frames;
            const auto count = std::min(maximum, frame.duration_samples - slice_start);
            const auto slice_end = slice_start + count;

            live_scene::Frame slice;
            slice.epoch_id = frame.epoch_id;
            slice.generation_id = frame.generation_id;
            slice.media_sample_start = frame.media_sample_start + static_cast<std::int64_t>(slice_start);
            slice.duration_samples = count;
            slice.flags = first_slice ? frame.flags : (frame.flags & ~live_scene::frame_discontinuity);
            slice.pcm.reserve(frame.pcm.size());
            for (const auto& plane : frame.pcm) {
                live_scene::PcmPlane sliced;
                sliced.element_id = plane.element_id;
                sliced.has_signal = plane.has_signal;
                if (plane.has_signal) {
                    const auto begin = plane.samples.begin() + static_cast<std::ptrdiff_t>(slice_start);
                    sliced.samples.assign(begin, begin + static_cast<std::ptrdiff_t>(count));
                }
                slice.pcm.push_back(std::move(sliced));
            }

            if (first_slice) {
                apply_initial_states(frame.initial_states, *switched, slice.initial_states);
            }
            const auto incoming_snapshot = incoming_renderer && incoming_needs_initial_state
                                               ? effective_state_snapshot()
                                               : std::vector<live_scene::StateEntry>{};
            std::uint64_t stream_order = 0U;
            append_policy_retargets(slice.updates, stream_order);
            while (update_index < frame.updates.size() && frame.updates[update_index].offset_samples < slice_end) {
                append_producer_update(frame.updates[update_index], slice_start, slice.updates, stream_order);
                ++update_index;
            }

            auto rendered = render_slice(slice, incoming_snapshot);
            if (!rendered) {
                return tl::unexpected{rendered.error()};
            }
            if (reset_or_quit(item.serial)) {
                return {};
            }
            auto fed = feed_rendered_slice(slice, item.serial);
            if (!fed) {
                return tl::unexpected{fed.error()};
            }
            slice_start = slice_end;
            first_slice = false;
        }
        return {};
    }

    void add_preroll_samples(std::uint64_t input_frames) noexcept {
        const std::uint64_t product = input_frames * static_cast<std::uint64_t>(config.output_sample_rate);
        const std::uint64_t numerator = preroll_rational_remainder + product;
        preroll_output_boundary += numerator / config.renderer.sample_rate;
        preroll_rational_remainder = numerator % config.renderer.sample_rate;
    }

    void finish_preroll() noexcept {
        if (target_reached) {
            return;
        }
        const auto boundary = preroll_output_boundary + (preroll_rational_remainder != 0U ? 1U : 0U);
        // libsamplerate may emit the final pre-target frames only after audible input arrives.
        output_frames_to_skip =
            boundary > preroll_output_frames_generated ? boundary - preroll_output_frames_generated : 0U;
        target_reached = true;
    }

    void add_timeline_samples(std::uint64_t input_frames) noexcept {
        const std::uint64_t product = input_frames * static_cast<std::uint64_t>(config.output_sample_rate);
        const std::uint64_t numerator = rational_remainder + product;
        allowed_output_frames += numerator / config.renderer.sample_rate;
        rational_remainder = numerator % config.renderer.sample_rate;
    }

    void apply_transition(float* samples, std::size_t frames, bool force_silence) noexcept {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (force_silence) {
                if (transition_remaining > 0U) {
                    ++transition_position;
                    --transition_remaining;
                }
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    samples[(frame * channels) + channel] = 0.0F;
                    last_output_frame[channel] = 0.0F;
                }
                continue;
            }
            if (transition_remaining > 0U) {
                const auto total = transition_remaining + transition_position;
                const float alpha = static_cast<float>(transition_position + 1U) / static_cast<float>(total);
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    const auto index = (frame * channels) + channel;
                    samples[index] = (transition_anchor[channel] * (1.0F - alpha)) + (samples[index] * alpha);
                }
                ++transition_position;
                --transition_remaining;
            }
            for (std::size_t channel = 0; channel < channels; ++channel) {
                last_output_frame[channel] = samples[(frame * channels) + channel];
            }
        }
    }

    void compact_pending() {
        if (pending_frame_offset == 0U) {
            return;
        }
        const auto total_frames = pending_output.size() / channels;
        if (pending_frame_offset >= total_frames) {
            pending_output.clear();
            pending_frame_offset = 0U;
            return;
        }
        if (pending_frame_offset >= k_resampler_output_frames) {
            const auto first = pending_output.begin() + static_cast<std::ptrdiff_t>(pending_frame_offset * channels);
            pending_output.erase(pending_output.begin(), first);
            pending_frame_offset = 0U;
        }
    }

    [[nodiscard]] Result<void> push_floats(const float* samples, std::uint64_t frames, std::uint64_t serial) {
        std::uint64_t offset = 0U;
        while (offset < frames) {
            if (reset_or_quit(serial)) {
                return {};
            }
            const auto lookahead = output_lookahead_frames();
            const auto buffered = player_output.buffered_frames();
            // Head tracking can lower the cap after an untracked partial fill.
            // Publish readiness even if no further push fits under that cap.
            publish_output_readiness(buffered, lookahead);
            if (buffered >= lookahead) {
                std::this_thread::sleep_for(k_ring_wait);
                continue;
            }
            const auto permitted_frames = std::min<std::uint64_t>(frames - offset, lookahead - buffered);
            const auto remaining_floats = static_cast<std::size_t>(permitted_frames) * channels;
            const auto pushed =
                player_output.push(samples + (static_cast<std::size_t>(offset) * channels), remaining_floats);
            const auto pushed_frames = pushed / channels;
            offset += pushed_frames;
            output_frames_pushed += pushed_frames;
            publish_output_readiness(player_output.buffered_frames(), lookahead);
            if (pushed_frames == 0U) {
                std::this_thread::sleep_for(k_ring_wait);
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> drain_pending(std::uint64_t serial) {
        while (output_frames_pushed < allowed_output_frames) {
            const auto total_frames = pending_output.size() / channels;
            if (pending_frame_offset >= total_frames) {
                break;
            }
            const auto available = total_frames - pending_frame_offset;
            const auto permitted = allowed_output_frames - output_frames_pushed;
            const auto frames = std::min<std::uint64_t>(available, permitted);
            auto pushed = push_floats(pending_output.data() + (pending_frame_offset * channels), frames, serial);
            if (!pushed) {
                return tl::unexpected{pushed.error()};
            }
            pending_frame_offset += static_cast<std::size_t>(frames);
            compact_pending();
        }
        return {};
    }

    [[nodiscard]] Result<void> accept_resampled(float* samples,
                                                std::size_t frames,
                                                bool collect_output,
                                                bool force_silence,
                                                bool before_target,
                                                std::uint64_t serial) {
        apply_transition(samples, frames, force_silence);
        if (before_target) {
            preroll_output_frames_generated += static_cast<std::uint64_t>(frames);
        }
        if (!collect_output || frames == 0U) {
            return {};
        }
        const auto skipped = std::min<std::uint64_t>(output_frames_to_skip, frames);
        samples += static_cast<std::size_t>(skipped) * channels;
        frames -= static_cast<std::size_t>(skipped);
        output_frames_to_skip -= skipped;
        if (frames == 0U) {
            return {};
        }
        compact_pending();
        pending_output.insert(pending_output.end(), samples, samples + (frames * channels));
        return drain_pending(serial);
    }

    [[nodiscard]] Result<void> feed_samples(const float* samples,
                                            std::uint32_t frames,
                                            bool count_timeline,
                                            bool collect_output,
                                            bool force_silence,
                                            bool before_target,
                                            std::uint64_t serial) {
        if (count_timeline) {
            add_timeline_samples(frames);
        }
        if (resampler == nullptr) {
            std::uint32_t offset = 0U;
            while (offset < frames) {
                const auto count = std::min<std::uint32_t>(frames - offset, k_resampler_output_frames);
                std::copy_n(samples + (static_cast<std::size_t>(offset) * channels),
                            static_cast<std::size_t>(count) * channels,
                            resample_output.data());
                auto accepted = accept_resampled(
                    resample_output.data(), count, collect_output, force_silence, before_target, serial);
                if (!accepted) {
                    return tl::unexpected{accepted.error()};
                }
                offset += count;
            }
            return {};
        }

        long remaining = frames;
        const float* input = samples;
        while (remaining > 0) {
            SRC_DATA request{};
            request.data_in = input;
            request.data_out = resample_output.data();
            request.input_frames = remaining;
            request.output_frames = k_resampler_output_frames;
            request.src_ratio =
                static_cast<double>(config.output_sample_rate) / static_cast<double>(config.renderer.sample_rate);
            request.end_of_input = 0;
            const int error = src_process(resampler.get(), &request);
            if (error != 0) {
                return make_error(ErrorCode::render_failed,
                                  std::string{"live Scene output resampling failed: "} + src_strerror(error));
            }
            auto accepted = accept_resampled(resample_output.data(),
                                             static_cast<std::size_t>(request.output_frames_gen),
                                             collect_output,
                                             force_silence,
                                             before_target,
                                             serial);
            if (!accepted) {
                return tl::unexpected{accepted.error()};
            }
            input += static_cast<std::size_t>(request.input_frames_used) * channels;
            remaining -= request.input_frames_used;
            if (request.input_frames_used == 0 && request.output_frames_gen == 0) {
                return make_error(ErrorCode::render_failed, "live Scene resampler made no progress");
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> flush_resampler(std::uint64_t serial) {
        if (resampler != nullptr) {
            while (true) {
                SRC_DATA request{};
                request.data_in = nullptr;
                request.data_out = resample_output.data();
                request.input_frames = 0;
                request.output_frames = k_resampler_output_frames;
                request.src_ratio =
                    static_cast<double>(config.output_sample_rate) / static_cast<double>(config.renderer.sample_rate);
                request.end_of_input = 1;
                const int error = src_process(resampler.get(), &request);
                if (error != 0) {
                    return make_error(ErrorCode::render_failed,
                                      std::string{"live Scene resampler flush failed: "} + src_strerror(error));
                }
                if (request.output_frames_gen == 0) {
                    break;
                }
                auto accepted = accept_resampled(resample_output.data(),
                                                 static_cast<std::size_t>(request.output_frames_gen),
                                                 true,
                                                 false,
                                                 false,
                                                 serial);
                if (!accepted) {
                    return tl::unexpected{accepted.error()};
                }
            }
        }
        return drain_pending(serial);
    }

    [[nodiscard]] Result<void> flush_backend(const WorkItem& item) {
        apply_available_controls();
        if (!has_current_generation) {
            return {};
        }
        std::uint32_t remaining = renderer->tail_input_frames();
        if (incoming_renderer) {
            remaining = std::max(remaining, incoming_renderer->tail_input_frames());
        }
        while (remaining > 0U) {
            if (reset_or_quit(item.serial)) {
                return {};
            }
            apply_available_controls();
            const auto maximum = head_tracking_recent() ? k_tracking_chunk_frames : k_worker_chunk_frames;
            const auto count = std::min(remaining, maximum);
            live_scene::Frame tail;
            tail.epoch_id = item.frame.epoch_id;
            tail.generation_id = current_generation;
            tail.duration_samples = count;
            tail.flags = live_scene::frame_state_complete;
            const auto incoming_snapshot = incoming_renderer && incoming_needs_initial_state
                                               ? effective_state_snapshot()
                                               : std::vector<live_scene::StateEntry>{};
            std::uint64_t stream_order = 0U;
            append_policy_retargets(tail.updates, stream_order);
            auto rendered = render_slice(tail, incoming_snapshot);
            if (!rendered) {
                return tl::unexpected{rendered.error()};
            }
            auto fed = feed_samples(render_output.data(), count, false, true, false, false, item.serial);
            if (!fed) {
                return tl::unexpected{fed.error()};
            }
            remaining -= count;
        }
        return {};
    }

    [[nodiscard]] Result<void> process_end(const WorkItem& item) {
        finish_preroll();
        if (rational_remainder != 0U) {
            ++allowed_output_frames;
            rational_remainder = 0U;
        }
        auto backend_flushed = flush_backend(item);
        if (!backend_flushed) {
            return tl::unexpected{backend_flushed.error()};
        }
        auto flushed = flush_resampler(item.serial);
        if (!flushed) {
            return tl::unexpected{flushed.error()};
        }
        while (output_frames_pushed < allowed_output_frames) {
            const auto count =
                std::min<std::uint64_t>(allowed_output_frames - output_frames_pushed, k_resampler_output_frames);
            auto padded = push_floats(zero_output.data(), count, item.serial);
            if (!padded) {
                return tl::unexpected{padded.error()};
            }
        }
        if (reset_or_quit(item.serial)) {
            return {};
        }
        production_done.store(true, std::memory_order_release);
        output_ready.store(true, std::memory_order_release);
        state.store(SceneStreamState::draining, std::memory_order_release);
        return {};
    }

    SceneStreamConfig config;
    std::unique_ptr<live_scene::ILiveSceneRenderer> renderer;
    const std::uint32_t input_sample_rate{0U};
    std::shared_ptr<DiagnosticStore> diagnostics;
    std::size_t channels{0U};
    PlayerOutputEngine player_output;
    SrcPtr resampler;

    std::thread worker;
    std::atomic<bool> quit{false};
    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<WorkItem> queue;
    std::unordered_map<std::uint64_t, std::shared_ptr<const Generation>> generations;
    std::unique_ptr<PendingBackend> pending_backend;
    bool orientation_pending{false};
    ListenerOrientation pending_orientation{};
    bool policy_pending{false};
    std::shared_ptr<const SemanticPolicy> pending_policy;
    std::uint64_t pending_policy_revision{0U};
    std::uint64_t reserved_samples{0U};
    std::uint64_t reserved_bytes{0U};
    bool has_epoch{false};
    std::uint64_t control_epoch{0U};
    std::int64_t control_target{0};
    std::int64_t expected_sample{0};
    bool has_expected_sample{false};
    bool producer_closed{true};
    std::uint64_t reset_request{0U};
    std::int64_t reset_target{0};
    std::uint64_t reset_ack{0U};
    std::atomic<std::uint64_t> reset_serial{0U};

    std::atomic<bool> reset_gate{false};
    std::atomic<std::uint32_t> active_pulls{0U};
    std::atomic<SceneStreamState> state{SceneStreamState::idle};
    std::atomic<std::uint64_t> epoch_status{0U};
    std::atomic<std::uint64_t> generation_status{0U};
    std::atomic<std::uint64_t> queued_samples{0U};
    std::atomic<std::uint64_t> queued_bytes{0U};
    std::atomic<std::uint64_t> media_frames_pulled{0U};
    std::atomic<std::uint64_t> underruns{0U};
    std::atomic<std::uint64_t> applied_policy_revision{0U};
    std::atomic<bool> output_ready{false};
    std::atomic<bool> output_attached{false};
    std::atomic<bool> production_done{false};
    std::atomic<bool> failed{false};

    std::int64_t target_sample_worker{0};
    std::uint64_t current_generation{0U};
    bool has_current_generation{false};
    std::shared_ptr<const Generation> current_generation_model;
    std::shared_ptr<const SemanticPolicy> current_policy;
    std::vector<ResolvedSemanticPolicy> resolved_policy;
    bool policy_retarget_pending{false};
    ListenerOrientation current_orientation{};
    std::atomic<std::int64_t> last_orientation_update_ns{0};
    std::unordered_map<std::uint64_t, live_scene::ObjectState> base_states;
    std::unordered_map<std::uint64_t, live_scene::ObjectState> effective_states;
    std::unordered_map<std::uint64_t, std::size_t> current_element_index;
    std::unique_ptr<live_scene::ILiveSceneRenderer> incoming_renderer;
    std::unique_ptr<PendingBackend> deferred_backend;
    live_scene::RendererConfig incoming_config;
    std::uint64_t backend_crossfade_position{0U};
    bool incoming_needs_initial_state{false};
    std::vector<float> render_output;
    std::vector<float> render_output_b;
    std::vector<float> resample_output;
    std::vector<float> zero_output;
    std::vector<float> pending_output;
    std::size_t pending_frame_offset{0U};
    std::uint64_t preroll_output_boundary{0U};
    std::uint64_t preroll_rational_remainder{0U};
    std::uint64_t preroll_output_frames_generated{0U};
    std::uint64_t output_frames_to_skip{0U};
    bool target_reached{false};
    std::uint64_t rational_remainder{0U};
    std::uint64_t allowed_output_frames{0U};
    std::uint64_t output_frames_pushed{0U};
    std::vector<float> last_output_frame;
    std::vector<float> transition_anchor;
    std::uint64_t transition_remaining{0U};
    std::uint64_t transition_position{0U};
};
// NOLINTEND(misc-non-private-member-variables-in-classes,clang-analyzer-optin.performance.Padding)

Result<std::unique_ptr<SceneStreamEngine>> SceneStreamEngine::create(SceneStreamConfig config) {
    if (config.renderer.sample_rate < k_min_sample_rate || config.renderer.sample_rate > k_max_sample_rate ||
        config.output_sample_rate < k_min_sample_rate || config.output_sample_rate > k_max_sample_rate) {
        return make_error(ErrorCode::invalid_argument, "Scene stream sample rates must be in the 8-192 kHz range");
    }
    config.input_queue_samples =
        config.input_queue_samples == 0U ? k_default_input_samples : config.input_queue_samples;
    config.input_queue_bytes = config.input_queue_bytes == 0U ? k_default_input_bytes : config.input_queue_bytes;
    config.output_ring_frames = config.output_ring_frames == 0U ? k_default_output_frames : config.output_ring_frames;
    config.startup_watermark_frames =
        config.startup_watermark_frames == 0U ? k_default_watermark_frames : config.startup_watermark_frames;
    if (config.input_queue_samples == 0U || config.input_queue_bytes == 0U || config.output_ring_frames == 0U ||
        config.startup_watermark_frames > config.output_ring_frames) {
        return make_error(ErrorCode::invalid_argument, "Scene stream queue/ring capacities are invalid");
    }

    auto diagnostics = std::make_shared<DiagnosticStore>();
    const live_scene::DiagnosticSink sink = [diagnostics](live_scene::Diagnostic diagnostic) {
        diagnostics->push(std::move(diagnostic));
    };
    auto renderer = create_live_renderer(config.renderer, sink);
    if (!renderer) {
        return tl::unexpected{renderer.error()};
    }
    if (*renderer == nullptr || (*renderer)->output_channels() == 0U) {
        return make_error(ErrorCode::internal_error, "live Scene renderer returned an invalid output format");
    }
    if ((*renderer)->output_channels() > std::numeric_limits<std::size_t>::max() / config.output_ring_frames) {
        return make_error(ErrorCode::invalid_argument, "Scene output ring size overflows addressable memory");
    }
    try {
        auto impl = std::make_unique<Impl>(std::move(config), std::move(*renderer), std::move(diagnostics));
        auto engine = std::unique_ptr<SceneStreamEngine>(new SceneStreamEngine(std::move(impl)));
        engine->impl_->start();
        return engine;
    } catch (const std::exception& exception) {
        return make_error(ErrorCode::internal_error,
                          std::string{"failed to create live Scene stream: "} + exception.what());
    } catch (...) {
        return make_error(ErrorCode::internal_error, "failed to create live Scene stream");
    }
}

SceneStreamEngine::SceneStreamEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SceneStreamEngine::~SceneStreamEngine() = default;

SceneOutputFormat SceneStreamEngine::output_format() const noexcept {
    return {impl_->config.output_sample_rate, static_cast<std::uint32_t>(impl_->channels)};
}

std::uint32_t SceneStreamEngine::input_sample_rate() const noexcept {
    return impl_->input_sample_rate;
}

Result<void> SceneStreamEngine::switch_backend(live_scene::RendererConfig config) {
    if (config.sample_rate != impl_->input_sample_rate) {
        return make_error(ErrorCode::unsupported, "live Scene backend switches cannot change the input sample rate");
    }
    const auto diagnostics = impl_->diagnostics;
    const live_scene::DiagnosticSink sink = [diagnostics](live_scene::Diagnostic diagnostic) {
        diagnostics->push(std::move(diagnostic));
    };
    auto prepared = create_live_renderer(config, sink);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    if (*prepared == nullptr || (*prepared)->output_channels() == 0U) {
        return make_error(ErrorCode::internal_error, "prepared live Scene renderer has an invalid output format");
    }
    if ((*prepared)->sample_rate() != impl_->input_sample_rate) {
        return make_error(ErrorCode::unsupported, "live Scene backend switches cannot change the renderer sample rate");
    }
    if ((*prepared)->output_channels() != impl_->channels) {
        return make_error(ErrorCode::unsupported, "live Scene backend switches cannot change the output channel count");
    }

    auto pending = std::make_unique<Impl::PendingBackend>();
    pending->config = std::move(config);
    pending->renderer = std::move(*prepared);
    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->pending_backend = std::move(pending);
    }
    impl_->queue_cv.notify_one();
    return {};
}

void SceneStreamEngine::set_listener_orientation(const ListenerOrientation& orientation) {
    impl_->last_orientation_update_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                            std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->pending_orientation = orientation;
        impl_->orientation_pending = true;
    }
    impl_->queue_cv.notify_one();
}

Result<void> SceneStreamEngine::set_semantic_policy_json(std::string_view json, std::uint64_t revision) {
    std::shared_ptr<const SemanticPolicy> parsed;
    if (!json.empty()) {
        auto policy = parse_semantic_policy(json, "<scene-stream>");
        if (!policy) {
            return tl::unexpected{policy.error()};
        }
        try {
            parsed = std::make_shared<const SemanticPolicy>(std::move(*policy));
        } catch (...) {
            return make_error(ErrorCode::internal_error, "failed to retain the live Scene semantic policy");
        }
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->pending_policy = std::move(parsed);
        impl_->pending_policy_revision = revision;
        impl_->policy_pending = true;
    }
    impl_->queue_cv.notify_one();
    return {};
}

Result<void> SceneStreamEngine::begin_epoch(std::uint64_t epoch_id, std::int64_t target_sample) {
    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        if (impl_->has_epoch && epoch_id <= impl_->control_epoch) {
            return make_error(ErrorCode::invalid_argument, "Scene epoch IDs must increase monotonically");
        }
    }

    impl_->reset_gate.store(true, std::memory_order_seq_cst);
    while (impl_->active_pulls.load(std::memory_order_seq_cst) != 0U) {
        std::this_thread::yield();
    }

    std::unique_lock<std::mutex> lock(impl_->queue_mutex);
    if (impl_->has_epoch && epoch_id <= impl_->control_epoch) {
        impl_->reset_gate.store(false, std::memory_order_seq_cst);
        return make_error(ErrorCode::invalid_argument, "Scene epoch IDs must increase monotonically");
    }
    const std::uint64_t serial = impl_->reset_serial.load(std::memory_order_relaxed) + 1U;
    impl_->reset_serial.store(serial, std::memory_order_release);
    impl_->queue.clear();
    impl_->generations.clear();
    impl_->reserved_samples = 0U;
    impl_->reserved_bytes = 0U;
    impl_->queued_samples.store(0U, std::memory_order_relaxed);
    impl_->queued_bytes.store(0U, std::memory_order_relaxed);
    impl_->has_epoch = true;
    impl_->control_epoch = epoch_id;
    impl_->control_target = target_sample;
    impl_->expected_sample = target_sample;
    impl_->has_expected_sample = false;
    impl_->producer_closed = false;
    impl_->reset_target = target_sample;
    impl_->epoch_status.store(epoch_id, std::memory_order_release);
    impl_->generation_status.store(0U, std::memory_order_release);
    impl_->state.store(SceneStreamState::buffering, std::memory_order_release);
    impl_->reset_request = serial;
    impl_->queue_cv.notify_all();
    impl_->queue_cv.wait(lock,
                         [&] { return impl_->reset_ack >= serial || impl_->quit.load(std::memory_order_acquire); });
    lock.unlock();
    impl_->reset_gate.store(false, std::memory_order_seq_cst);
    if (impl_->quit.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::internal_error, "Scene stream closed while resetting its epoch");
    }
    if (impl_->failed.load(std::memory_order_acquire)) {
        return make_error(ErrorCode::internal_error, "Scene stream worker failed while resetting its epoch");
    }
    return {};
}

Result<void> SceneStreamEngine::configure_generation(std::uint64_t epoch_id,
                                                     std::uint64_t generation_id,
                                                     std::span<const live_scene::ElementDescriptor> elements) {
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(elements.size());
    for (const auto& element : elements) {
        if (element.role != live_scene::ElementRole::object &&
            element.role != live_scene::ElementRole::direct_speaker && element.role != live_scene::ElementRole::lfe) {
            return make_error(ErrorCode::invalid_argument, "Scene generation contains an invalid element role");
        }
        if (!ids.insert(element.element_id).second) {
            return make_error(ErrorCode::invalid_argument, "Scene generation contains a duplicate element ID");
        }
        if (element.has_position &&
            (!std::isfinite(element.x) || !std::isfinite(element.y) || !std::isfinite(element.z) || element.x < -1.0F ||
             element.x > 1.0F || element.y < -1.0F || element.y > 1.0F || element.z < -1.0F || element.z > 1.0F)) {
            return make_error(ErrorCode::invalid_argument,
                              "Scene element Cartesian position must use finite canonical [-1, 1] coordinates");
        }
    }

    try {
        auto generation = std::make_shared<Impl::Generation>();
        generation->id = generation_id;
        generation->elements.assign(elements.begin(), elements.end());
        auto identities = build_policy_identities(generation->elements);
        if (!identities) {
            return tl::unexpected{identities.error()};
        }
        generation->policy_identities = std::move(*identities);
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        if (!impl_->has_epoch || epoch_id != impl_->control_epoch) {
            return make_error(ErrorCode::invalid_argument, "Scene generation uses an unknown epoch");
        }
        if (impl_->producer_closed) {
            return make_error(ErrorCode::invalid_argument, "Scene epoch is closed");
        }
        if (const auto found = impl_->generations.find(generation_id); found != impl_->generations.end()) {
            if (found->second->elements.size() != elements.size() || !std::equal(found->second->elements.begin(),
                                                                                 found->second->elements.end(),
                                                                                 elements.begin(),
                                                                                 descriptor_equal)) {
                return make_error(ErrorCode::invalid_argument,
                                  "Scene generation topology cannot change after configuration");
            }
            return {};
        }
        impl_->generations.emplace(generation_id, std::move(generation));
        return {};
    } catch (...) {
        return make_error(ErrorCode::internal_error, "failed to copy Scene generation descriptors");
    }
}

// Validation, budget reservation, and deep-copy form one transaction; keeping them together makes
// rollback and the "accepted means owned" ABI guarantee auditable.
// NOLINTNEXTLINE(readability-function-size)
Result<SceneSubmitStatus> SceneStreamEngine::submit_frame(const SceneFrameView& view,
                                                          std::chrono::milliseconds timeout) {
    if (view.duration_samples == 0U ||
        view.media_sample_start > std::numeric_limits<std::int64_t>::max() - view.duration_samples) {
        return make_error(ErrorCode::invalid_argument, "Scene frame duration/timeline is invalid");
    }
    constexpr std::uint32_t k_known_flags = live_scene::frame_state_complete | live_scene::frame_warmup |
                                            live_scene::frame_discontinuity | live_scene::frame_concealed;
    if ((view.flags & ~k_known_flags) != 0U) {
        return make_error(ErrorCode::invalid_argument, "Scene frame has unknown flags");
    }

    std::shared_ptr<const Impl::Generation> generation;
    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        if (!impl_->has_epoch || view.epoch_id != impl_->control_epoch) {
            return make_error(ErrorCode::invalid_argument, "Scene frame uses an unknown epoch");
        }
        const auto found = impl_->generations.find(view.generation_id);
        if (found == impl_->generations.end()) {
            return make_error(ErrorCode::invalid_argument, "Scene frame uses an unknown generation");
        }
        generation = found->second;
    }

    if (view.pcm.size() != generation->elements.size()) {
        return make_error(ErrorCode::invalid_argument, "Scene frame must provide one PCM plane per element");
    }
    std::unordered_set<std::uint64_t> element_ids;
    element_ids.reserve(generation->elements.size());
    for (const auto& element : generation->elements) {
        element_ids.insert(element.element_id);
    }
    std::unordered_set<std::uint64_t> plane_ids;
    plane_ids.reserve(view.pcm.size());
    std::size_t owned_bytes = sizeof(live_scene::Frame);
    if (!add_size(owned_bytes, view.pcm.size(), sizeof(live_scene::PcmPlane)) ||
        !add_size(owned_bytes, view.initial_states.size(), sizeof(live_scene::StateEntry)) ||
        !add_size(owned_bytes, view.updates.size(), sizeof(live_scene::MetadataUpdate))) {
        return make_error(ErrorCode::invalid_argument, "Scene frame owned size overflows addressable memory");
    }
    for (const auto& plane : view.pcm) {
        if (!plane_ids.insert(plane.element_id).second || !element_ids.contains(plane.element_id)) {
            return make_error(ErrorCode::invalid_argument, "Scene PCM plane has a duplicate or unknown element ID");
        }
        if (plane.sample_count != view.duration_samples) {
            return make_error(ErrorCode::invalid_argument, "Scene PCM plane sample count must equal frame duration");
        }
        if (plane.has_signal && (plane.samples == nullptr || plane.stride == 0U)) {
            return make_error(ErrorCode::invalid_argument, "signalled Scene PCM plane has no usable samples");
        }
        if (plane.has_signal && view.duration_samples > 1U &&
            static_cast<std::uint64_t>(plane.stride) * (view.duration_samples - 1U) >
                std::numeric_limits<std::size_t>::max() / sizeof(float)) {
            return make_error(ErrorCode::invalid_argument, "Scene PCM stride overflows addressable memory");
        }
        if (!add_size(owned_bytes, plane.has_signal ? view.duration_samples : 0U, sizeof(float))) {
            return make_error(ErrorCode::invalid_argument, "Scene PCM owned size overflows addressable memory");
        }
        if (plane.has_signal) {
            for (std::uint32_t sample = 0U; sample < view.duration_samples; ++sample) {
                if (!std::isfinite(plane.samples[static_cast<std::size_t>(sample) * plane.stride])) {
                    return make_error(ErrorCode::invalid_argument, "Scene PCM samples must be finite");
                }
            }
        }
    }
    std::unordered_set<std::uint64_t> initial_ids;
    const bool invalid_initial_state = std::ranges::any_of(view.initial_states, [&](const auto& initial) {
        return !element_ids.contains(initial.element_id) || !initial_ids.insert(initial.element_id).second ||
               !finite_state(initial.state);
    });
    if (invalid_initial_state) {
        return make_error(ErrorCode::invalid_argument,
                          "Scene initial state has an unknown/duplicate element or invalid value");
    }
    const bool invalid_update = std::ranges::any_of(view.updates, [&](const auto& update) {
        return !element_ids.contains(update.element_id) || update.offset_samples >= view.duration_samples ||
               update.changed_fields == 0U || (update.changed_fields & ~live_scene::k_known_state_fields) != 0U ||
               (update.changed_fields & ~update.state.valid_fields) != 0U || !finite_state(update.state);
    });
    if (invalid_update) {
        return make_error(ErrorCode::invalid_argument, "Scene metadata update is invalid");
    }
    if (view.duration_samples > impl_->config.input_queue_samples || owned_bytes > impl_->config.input_queue_bytes) {
        return make_error(ErrorCode::invalid_argument, "Scene frame exceeds the configured input queue budget");
    }

    std::uint64_t serial = 0U;
    {
        std::unique_lock<std::mutex> lock(impl_->queue_mutex);
        const auto timeline_valid = [&] {
            return impl_->has_expected_sample ? view.media_sample_start == impl_->expected_sample
                                              : view.media_sample_start <= impl_->control_target;
        };
        if ((view.flags & live_scene::frame_discontinuity) != 0U && impl_->has_expected_sample) {
            return make_error(ErrorCode::invalid_argument,
                              "Scene discontinuity flag is only valid on the first frame after begin_epoch");
        }
        if (!timeline_valid()) {
            return make_error(ErrorCode::invalid_argument, "Scene frame creates a timeline gap or overlap");
        }
        const auto capacity_available = [&] {
            return impl_->reserved_samples + view.duration_samples <= impl_->config.input_queue_samples &&
                   impl_->reserved_bytes + owned_bytes <= impl_->config.input_queue_bytes;
        };
        const auto closed = [&] {
            return impl_->producer_closed || impl_->failed.load(std::memory_order_acquire) ||
                   impl_->quit.load(std::memory_order_acquire) || view.epoch_id != impl_->control_epoch;
        };
        if (closed()) {
            return SceneSubmitStatus::closed;
        }
        if (!capacity_available()) {
            if (timeout.count() == 0) {
                return SceneSubmitStatus::would_block;
            }
            const bool awakened =
                impl_->queue_cv.wait_for(lock, timeout, [&] { return capacity_available() || closed(); });
            if (!awakened) {
                return SceneSubmitStatus::timed_out;
            }
            if (closed()) {
                return SceneSubmitStatus::closed;
            }
        }
        serial = impl_->reset_serial.load(std::memory_order_relaxed);
        impl_->reserved_samples += view.duration_samples;
        impl_->reserved_bytes += owned_bytes;
        impl_->queued_samples.store(impl_->reserved_samples, std::memory_order_relaxed);
        impl_->queued_bytes.store(impl_->reserved_bytes, std::memory_order_relaxed);
    }

    Impl::WorkItem item;
    try {
        item.kind = Impl::WorkItem::Kind::frame;
        item.serial = serial;
        item.generation = std::move(generation);
        item.reserved_samples = view.duration_samples;
        item.reserved_bytes = owned_bytes;
        item.frame.epoch_id = view.epoch_id;
        item.frame.generation_id = view.generation_id;
        item.frame.media_sample_start = view.media_sample_start;
        item.frame.duration_samples = view.duration_samples;
        item.frame.flags = view.flags;
        item.frame.pcm.reserve(view.pcm.size());
        for (const auto& plane : view.pcm) {
            live_scene::PcmPlane owned;
            owned.element_id = plane.element_id;
            owned.has_signal = plane.has_signal;
            if (plane.has_signal) {
                owned.samples.resize(view.duration_samples);
                for (std::uint32_t sample = 0U; sample < view.duration_samples; ++sample) {
                    owned.samples[sample] = plane.samples[static_cast<std::size_t>(sample) * plane.stride];
                }
            }
            item.frame.pcm.push_back(std::move(owned));
        }
        item.frame.initial_states.assign(view.initial_states.begin(), view.initial_states.end());
        item.frame.updates.assign(view.updates.begin(), view.updates.end());
        for (std::size_t index = 0U; index < item.frame.updates.size(); ++index) {
            item.frame.updates[index].stream_order = index;
        }
        std::ranges::stable_sort(item.frame.updates, [](const auto& lhs, const auto& rhs) {
            return lhs.offset_samples < rhs.offset_samples;
        });
    } catch (...) {
        impl_->release_budget(item);
        return make_error(ErrorCode::internal_error, "failed to deep-copy Scene frame");
    }

    {
        const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        if (impl_->producer_closed || serial != impl_->reset_serial.load(std::memory_order_relaxed)) {
            impl_->reserved_samples -= std::min(impl_->reserved_samples, item.reserved_samples);
            impl_->reserved_bytes -= std::min(impl_->reserved_bytes, item.reserved_bytes);
            impl_->queued_samples.store(impl_->reserved_samples, std::memory_order_relaxed);
            impl_->queued_bytes.store(impl_->reserved_bytes, std::memory_order_relaxed);
            return SceneSubmitStatus::closed;
        }
        try {
            impl_->queue.push_back(std::move(item));
        } catch (...) {
            impl_->reserved_samples -= view.duration_samples;
            impl_->reserved_bytes -= owned_bytes;
            impl_->queued_samples.store(impl_->reserved_samples, std::memory_order_relaxed);
            impl_->queued_bytes.store(impl_->reserved_bytes, std::memory_order_relaxed);
            return make_error(ErrorCode::internal_error, "failed to enqueue Scene frame");
        }
        impl_->expected_sample = view.media_sample_start + static_cast<std::int64_t>(view.duration_samples);
        impl_->has_expected_sample = true;
    }
    impl_->queue_cv.notify_one();
    return SceneSubmitStatus::accepted;
}

Result<void> SceneStreamEngine::signal_end(std::uint64_t epoch_id, std::int64_t end_sample) {
    const std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    if (!impl_->has_epoch || epoch_id != impl_->control_epoch) {
        return make_error(ErrorCode::invalid_argument, "Scene end marker uses an unknown epoch");
    }
    if (impl_->producer_closed) {
        return make_error(ErrorCode::invalid_argument, "Scene epoch is already closed");
    }
    const std::int64_t expected = impl_->has_expected_sample ? impl_->expected_sample : impl_->control_target;
    if (end_sample != expected) {
        return make_error(ErrorCode::invalid_argument, "Scene end marker does not match the next expected sample");
    }
    Impl::WorkItem item;
    item.kind = Impl::WorkItem::Kind::end;
    item.serial = impl_->reset_serial.load(std::memory_order_relaxed);
    item.frame.epoch_id = epoch_id;
    try {
        impl_->queue.push_back(std::move(item));
    } catch (...) {
        return make_error(ErrorCode::internal_error, "failed to enqueue Scene end marker");
    }
    impl_->producer_closed = true;
    impl_->state.store(SceneStreamState::draining, std::memory_order_release);
    impl_->queue_cv.notify_one();
    return {};
}

bool SceneStreamEngine::attach_output() noexcept {
    bool expected = false;
    return impl_->output_attached.compare_exchange_strong(expected, true);
}

void SceneStreamEngine::detach_output() noexcept {
    impl_->output_attached.store(false);
}

bool SceneStreamEngine::output_attached() const noexcept {
    return impl_->output_attached.load();
}

ScenePullResult SceneStreamEngine::pull(float* output, std::uint32_t frames) noexcept {
    ScenePullResult result;
    result.requested_frames = frames;
    result.epoch_id = impl_->epoch_status.load(std::memory_order_acquire);
    result.first_media_frame = impl_->media_frames_pulled.load(std::memory_order_relaxed);
    if (frames == 0U) {
        return result;
    }
    std::fill_n(output, static_cast<std::size_t>(frames) * impl_->channels, 0.0F);
    if (impl_->reset_gate.load(std::memory_order_seq_cst)) {
        result.flags |= scene_pull_buffering;
        return result;
    }
    impl_->active_pulls.fetch_add(1U, std::memory_order_seq_cst);
    if (impl_->reset_gate.load(std::memory_order_seq_cst)) {
        impl_->active_pulls.fetch_sub(1U, std::memory_order_seq_cst);
        result.flags |= scene_pull_buffering;
        return result;
    }

    const auto available_frames = impl_->player_output.buffered_frames();
    const bool done = impl_->production_done.load(std::memory_order_acquire);
    bool ready = impl_->output_ready.load(std::memory_order_acquire);
    if (!ready && (available_frames >= impl_->effective_startup_watermark() || done)) {
        impl_->output_ready.store(true, std::memory_order_release);
        ready = true;
    }
    if (!ready) {
        result.flags |= scene_pull_buffering;
    } else {
        const auto popped = impl_->player_output.pop(output, static_cast<std::size_t>(frames) * impl_->channels);
        result.media_frames = static_cast<std::uint32_t>(popped / impl_->channels);
        if (result.media_frames > 0U) {
            impl_->media_frames_pulled.fetch_add(result.media_frames, std::memory_order_relaxed);
        }
        if (result.media_frames < frames) {
            if (done && impl_->player_output.available_floats() == 0U &&
                !impl_->failed.load(std::memory_order_acquire)) {
                result.flags |= scene_pull_eos;
                impl_->state.store(SceneStreamState::ended, std::memory_order_release);
            } else if (!impl_->failed.load(std::memory_order_acquire)) {
                result.flags |= scene_pull_underrun;
                impl_->underruns.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
    if (impl_->failed.load(std::memory_order_acquire)) {
        result.flags |= scene_pull_failed;
    }
    impl_->active_pulls.fetch_sub(1U, std::memory_order_seq_cst);
    return result;
}

SceneStreamStatus SceneStreamEngine::status() const noexcept {
    SceneStreamStatus result;
    result.state = impl_->state.load(std::memory_order_acquire);
    result.epoch_id = impl_->epoch_status.load(std::memory_order_acquire);
    result.generation_id = impl_->generation_status.load(std::memory_order_acquire);
    result.queued_input_samples = impl_->queued_samples.load(std::memory_order_relaxed);
    result.queued_input_bytes = impl_->queued_bytes.load(std::memory_order_relaxed);
    result.buffered_output_frames = impl_->player_output.buffered_frames();
    result.media_frames_pulled = impl_->media_frames_pulled.load(std::memory_order_relaxed);
    result.underruns = impl_->underruns.load(std::memory_order_relaxed);
    result.semantic_degradations = impl_->diagnostics->degradation_count();
    result.semantic_policy_revision = impl_->applied_policy_revision.load(std::memory_order_acquire);
    result.ring_fill = impl_->player_output.capacity_frames() == 0U
                           ? 0.0F
                           : static_cast<float>(result.buffered_output_frames) /
                                 static_cast<float>(impl_->player_output.capacity_frames());
    result.ended = result.state == SceneStreamState::ended;
    result.failed = impl_->failed.load(std::memory_order_acquire);
    return result;
}

std::size_t SceneStreamEngine::diagnostic_count() const noexcept {
    return impl_->diagnostics->size();
}

std::optional<live_scene::Diagnostic> SceneStreamEngine::diagnostic(std::size_t index) const {
    return impl_->diagnostics->at(index);
}

} // namespace mradm::realtime
