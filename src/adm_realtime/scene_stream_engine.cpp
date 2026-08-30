#include "scene_stream_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
           lhs.flags == rhs.flags;
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
    return true;
}

[[nodiscard]] bool add_size(std::size_t& total, std::size_t count, std::size_t item_size) noexcept {
    if (count > (std::numeric_limits<std::size_t>::max() - total) / item_size) {
        return false;
    }
    total += count * item_size;
    return true;
}

} // namespace

// NOLINTBEGIN(misc-non-private-member-variables-in-classes,clang-analyzer-optin.performance.Padding)
struct SceneStreamEngine::Impl {
    struct Generation {
        std::uint64_t id{0U};
        std::vector<live_scene::ElementDescriptor> elements;
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
          diagnostics(std::move(diagnostic_store)), channels(renderer->output_channels()),
          player_output(channels, config.output_ring_frames),
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

    void fail(const Error& error, std::uint64_t epoch_id, std::uint64_t generation_id) {
        add_diagnostic({LogLevel::error,
                        live_scene::DiagnosticCode::backend_failure,
                        epoch_id,
                        generation_id,
                        0U,
                        0U,
                        error.message + (error.context.empty() ? std::string{} : ": " + error.context)});
        failed.store(true, std::memory_order_release);
        production_done.store(true, std::memory_order_release);
        output_ready.store(true, std::memory_order_release);
        state.store(SceneStreamState::failed, std::memory_order_release);
        {
            const std::lock_guard<std::mutex> lock(queue_mutex);
            producer_closed = true;
            queue.clear();
            reserved_samples = 0U;
            reserved_bytes = 0U;
            queued_samples.store(0U, std::memory_order_relaxed);
            queued_bytes.store(0U, std::memory_order_relaxed);
        }
        queue_cv.notify_all();
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

    void perform_reset(std::uint64_t serial, std::int64_t target_sample) {
        (*renderer).reset();
        if (resampler != nullptr) {
            src_reset(resampler.get());
        }
        player_output.clear();
        pending_output.clear();
        pending_frame_offset = 0U;
        current_generation = 0U;
        target_sample_worker = target_sample;
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

    void worker_loop() {
        while (!quit.load(std::memory_order_acquire)) {
            WorkItem item;
            bool have_item = false;
            std::uint64_t pending_reset = 0U;
            std::int64_t pending_target = 0;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait(lock, [&] {
                    return quit.load(std::memory_order_acquire) || reset_request != 0U || !queue.empty();
                });
                if (quit.load(std::memory_order_acquire)) {
                    return;
                }
                if (reset_request != 0U) {
                    pending_reset = reset_request;
                    pending_target = reset_target;
                    reset_request = 0U;
                } else if (!queue.empty()) {
                    item = std::move(queue.front());
                    queue.pop_front();
                    have_item = true;
                }
            }
            if (pending_reset != 0U) {
                perform_reset(pending_reset, pending_target);
                continue;
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
                fail(processed.error(),
                     item.frame.epoch_id != 0U ? item.frame.epoch_id : epoch_status.load(),
                     item.frame.generation_id);
            }
        }
    }

    [[nodiscard]] Result<void> switch_generation(const WorkItem& item) {
        if (item.generation == nullptr) {
            return make_error(ErrorCode::internal_error, "Scene frame lost its generation descriptor");
        }
        if (current_generation == item.generation->id) {
            return {};
        }
        auto configured = renderer->configure_generation(item.generation->id, item.generation->elements);
        if (!configured) {
            return tl::unexpected{configured.error()};
        }
        current_generation = item.generation->id;
        generation_status.store(current_generation, std::memory_order_release);
        transition_anchor = last_output_frame;
        transition_remaining = std::max<std::uint64_t>(
            1U, (static_cast<std::uint64_t>(config.output_sample_rate) * k_generation_declick_ms) / 1000U);
        transition_position = 0U;
        return {};
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
        render_output.resize(static_cast<std::size_t>(frame.duration_samples) * channels);
        auto rendered = renderer->render(frame, render_output);
        if (!rendered) {
            return tl::unexpected{rendered.error()};
        }
        if (reset_or_quit(item.serial)) {
            return {};
        }

        const auto frame_end = frame.media_sample_start + static_cast<std::int64_t>(frame.duration_samples);
        std::uint32_t hidden_frames = 0U;
        if (frame.media_sample_start < target_sample_worker) {
            const auto hidden_end = std::min(frame_end, target_sample_worker);
            hidden_frames = static_cast<std::uint32_t>(hidden_end - frame.media_sample_start);
        }
        if (hidden_frames > 0U) {
            auto hidden = feed_samples(render_output.data(), hidden_frames, false, false, false, item.serial);
            if (!hidden) {
                return tl::unexpected{hidden.error()};
            }
        }
        if (hidden_frames == frame.duration_samples) {
            return {};
        }

        const auto audible_frames = frame.duration_samples - hidden_frames;
        const bool state_complete = (frame.flags & live_scene::frame_state_complete) != 0U;
        const bool warmup = (frame.flags & live_scene::frame_warmup) != 0U;
        float* audible = render_output.data() + (static_cast<std::size_t>(hidden_frames) * channels);
        const bool force_silence = !state_complete || warmup;
        if (force_silence) {
            std::fill_n(audible, static_cast<std::size_t>(audible_frames) * channels, 0.0F);
        }
        return feed_samples(audible, audible_frames, true, true, force_silence, item.serial);
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
            const auto remaining_floats = static_cast<std::size_t>(frames - offset) * channels;
            const auto pushed =
                player_output.push(samples + (static_cast<std::size_t>(offset) * channels), remaining_floats);
            const auto pushed_frames = pushed / channels;
            offset += pushed_frames;
            output_frames_pushed += pushed_frames;
            if (player_output.buffered_frames() >= config.startup_watermark_frames) {
                output_ready.store(true, std::memory_order_release);
                state.store(SceneStreamState::running, std::memory_order_release);
            }
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

    [[nodiscard]] Result<void> accept_resampled(
        float* samples, std::size_t frames, bool collect_output, bool force_silence, std::uint64_t serial) {
        apply_transition(samples, frames, force_silence);
        if (!collect_output || frames == 0U) {
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
                auto accepted = accept_resampled(resample_output.data(), count, collect_output, force_silence, serial);
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
                auto accepted = accept_resampled(
                    resample_output.data(), static_cast<std::size_t>(request.output_frames_gen), true, false, serial);
                if (!accepted) {
                    return tl::unexpected{accepted.error()};
                }
            }
        }
        return drain_pending(serial);
    }

    [[nodiscard]] Result<void> flush_backend(const WorkItem& item) {
        std::uint32_t remaining = renderer->tail_input_frames();
        if (remaining == 0U || current_generation == 0U) {
            return {};
        }
        while (remaining > 0U) {
            if (reset_or_quit(item.serial)) {
                return {};
            }
            const auto count = std::min(remaining, k_resampler_output_frames);
            live_scene::Frame tail;
            tail.epoch_id = item.frame.epoch_id;
            tail.generation_id = current_generation;
            tail.duration_samples = count;
            tail.flags = live_scene::frame_state_complete;
            render_output.resize(static_cast<std::size_t>(count) * channels);
            auto rendered = renderer->render(tail, render_output);
            if (!rendered) {
                return tl::unexpected{rendered.error()};
            }
            auto fed = feed_samples(render_output.data(), count, false, true, false, item.serial);
            if (!fed) {
                return tl::unexpected{fed.error()};
            }
            remaining -= count;
        }
        return {};
    }

    [[nodiscard]] Result<void> process_end(const WorkItem& item) {
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
    std::atomic<bool> output_ready{false};
    std::atomic<bool> production_done{false};
    std::atomic<bool> failed{false};

    std::int64_t target_sample_worker{0};
    std::uint64_t current_generation{0U};
    std::vector<float> render_output;
    std::vector<float> resample_output;
    std::vector<float> zero_output;
    std::vector<float> pending_output;
    std::size_t pending_frame_offset{0U};
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
    Result<std::unique_ptr<live_scene::ILiveSceneRenderer>> renderer = [&]() {
        if (config.renderer.renderer == RendererSelection::saf) {
            return live_scene::create_live_vbap_renderer(config.renderer, sink);
        }
        if (config.renderer.renderer == RendererSelection::saf_binaural) {
            return live_scene::create_live_binaural_renderer(config.renderer, sink);
        }
        return Result<std::unique_ptr<live_scene::ILiveSceneRenderer>>{
            make_error(ErrorCode::unsupported, "Scene stream v1 supports only SAF VBAP and SAF binaural renderers")};
    }();
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
    if (!ready && (available_frames >= impl_->config.startup_watermark_frames || done)) {
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
