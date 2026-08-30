#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "adm/errors.h"

#include "../adm_render_common/live_scene_renderer.h"

namespace mradm::realtime {

// These private cross-translation-unit aggregates are consumed by the C ABI implementation and
// Scene worker. cppcheck analyzes this header in isolation and cannot observe those uses.
// cppcheck-suppress-begin unusedStructMember
struct SceneStreamConfig {
    live_scene::RendererConfig renderer;
    std::uint32_t output_sample_rate{48000U};
    std::uint32_t input_queue_samples{32768U};
    std::uint64_t input_queue_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint32_t output_ring_frames{8192U};
    std::uint32_t startup_watermark_frames{4096U};
};

struct ScenePcmPlaneView {
    std::uint64_t element_id{0};
    const float* samples{nullptr};
    std::uint32_t sample_count{0};
    std::uint32_t stride{1};
    bool has_signal{false};
};

struct SceneFrameView {
    std::uint64_t epoch_id{0};
    std::uint64_t generation_id{0};
    std::int64_t media_sample_start{0};
    std::uint32_t duration_samples{0};
    std::uint32_t flags{0};
    std::span<const ScenePcmPlaneView> pcm;
    std::span<const live_scene::StateEntry> initial_states;
    std::span<const live_scene::MetadataUpdate> updates;
};

enum class SceneSubmitStatus : std::uint8_t { accepted = 0, would_block = 1, timed_out = 2, closed = 3 };
enum class SceneStreamState : std::uint8_t {
    idle = 0,
    buffering = 1,
    running = 2,
    draining = 3,
    ended = 4,
    failed = 5
};

enum ScenePullFlag : std::uint32_t {
    scene_pull_buffering = 1U << 0U,
    scene_pull_underrun = 1U << 1U,
    scene_pull_eos = 1U << 2U,
    scene_pull_failed = 1U << 3U,
};

struct SceneOutputFormat {
    std::uint32_t sample_rate{0};
    std::uint32_t channels{0};
};

struct ScenePullResult {
    std::uint32_t flags{0};
    std::uint64_t epoch_id{0};
    std::uint64_t first_media_frame{0};
    std::uint32_t media_frames{0};
    std::uint32_t requested_frames{0};
};

struct SceneStreamStatus {
    SceneStreamState state{SceneStreamState::idle};
    std::uint64_t epoch_id{0};
    std::uint64_t generation_id{0};
    std::uint64_t queued_input_samples{0};
    std::uint64_t queued_input_bytes{0};
    std::uint64_t buffered_output_frames{0};
    std::uint64_t media_frames_pulled{0};
    std::uint64_t underruns{0};
    std::uint64_t semantic_degradations{0};
    float ring_fill{0.0F};
    bool ended{false};
    bool failed{false};
};

class SceneStreamEngine {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SceneStreamEngine>> create(SceneStreamConfig config);

    ~SceneStreamEngine();
    SceneStreamEngine(const SceneStreamEngine&) = delete;
    SceneStreamEngine& operator=(const SceneStreamEngine&) = delete;
    SceneStreamEngine(SceneStreamEngine&&) = delete;
    SceneStreamEngine& operator=(SceneStreamEngine&&) = delete;

    [[nodiscard]] SceneOutputFormat output_format() const noexcept;
    [[nodiscard]] Result<void> begin_epoch(std::uint64_t epoch_id, std::int64_t target_sample);
    [[nodiscard]] Result<void> configure_generation(std::uint64_t epoch_id,
                                                    std::uint64_t generation_id,
                                                    std::span<const live_scene::ElementDescriptor> elements);
    [[nodiscard]] Result<SceneSubmitStatus> submit_frame(const SceneFrameView& view, std::chrono::milliseconds timeout);
    [[nodiscard]] Result<void> signal_end(std::uint64_t epoch_id, std::int64_t end_sample);

    [[nodiscard]] ScenePullResult pull(float* interleaved_output, std::uint32_t frames) noexcept;
    [[nodiscard]] SceneStreamStatus status() const noexcept;

    [[nodiscard]] std::size_t diagnostic_count() const noexcept;
    [[nodiscard]] std::optional<live_scene::Diagnostic> diagnostic(std::size_t index) const;

  private:
    struct Impl;
    explicit SceneStreamEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
// cppcheck-suppress-end unusedStructMember

} // namespace mradm::realtime
