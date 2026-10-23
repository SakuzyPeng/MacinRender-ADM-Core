#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/direct_speakers_matrix.h"
#include "adm/live_override.h"
#include "adm/logging.h"
#include "adm/render.h"
#include "adm/scene.h"

#include "../adm_dsp/gain.h"
#include "routing_ffi.h"

namespace mradm::render_common {

// Blocks of one track sharing a start: the last one in scene (ADM document) order is in effect
// for its entire interval, even when it ends before its peers. Resolve this before a backend
// splits blocks into lanes (point / cloud / diffuse / spreader sources, LFE or divergence buses)
// or interpolates from a predecessor, so an overwritten block leaves no lane, ramp or routing
// decision behind. The result is ordered by start (stable for equal starts before the collapse).
template <typename Block>
[[nodiscard]] std::vector<const Block*> last_blocks_at_each_start(const std::vector<Block>& blocks) {
    std::vector<const Block*> ordered;
    ordered.reserve(blocks.size());
    std::ranges::transform(blocks, std::back_inserter(ordered), [](const auto& block) { return &block; });
    std::ranges::stable_sort(ordered, {}, &Block::start_sample);
    std::size_t count = 0;
    for (const auto* block : ordered) {
        if (count > 0 && ordered[count - 1]->start_sample == block->start_sample) {
            ordered[count - 1] = block;
        } else {
            ordered[count++] = block;
        }
    }
    ordered.resize(count);
    return ordered;
}

// Alphanumeric-only speaker-label key (uppercased; '+', '-' and separators dropped). Used for
// LFE keyword detection where the sign / position digits do not matter (LFE / LFE1 / SUB …).
[[nodiscard]] std::string normalise_speaker_label_key(std::string_view raw);

// Canonical speaker-label key for per-channel matching: keep alphanumerics AND '+' / '-',
// uppercased. Unlike normalise_speaker_label_key this PRESERVES the sign, so "M+030" and "M-030"
// (left vs right) stay distinct while "m+030" / "M+030" / "M_+030" compare equal. Used for the
// per-channel live-gain match (channel key + override speaker_label canonicalised the same way).
[[nodiscard]] std::string canonicalise_speaker_label(std::string_view raw);

// Canonical label used by matrix source/target matching. In addition to the
// punctuation/case normalisation above, known RoomCentric / DAW aliases are
// converted to their BS.2051 label (for example L -> M+030).
[[nodiscard]] std::string canonical_direct_speaker_label(std::string_view raw);

[[nodiscard]] std::string_view direct_speakers_routing_mode_name(DirectSpeakersRoutingMode mode) noexcept;

// Backend-neutral output slot used by shared DirectSpeakers label/fallback
// routing. Backends adapt their native layout representation into this view.
struct DirectSpeakerRoutingTarget {
    std::string_view label;
    float azimuth{0.0F};
    float elevation{0.0F};
    bool is_lfe{false};
};

// Resolve a DirectSpeakers label in two passes: first exact output-label
// matching, then the project's established RoomCentric/DAW alias table.
[[nodiscard]] std::optional<std::size_t>
direct_speaker_index_for_labels(std::span<const DirectSpeakerRoutingTarget> targets,
                                const std::vector<std::string>& labels);

struct ResolvedDirectSpeakersMatrixTarget {
    std::size_t output_channel{0};
    float gain{0.0F};
};

struct ResolvedDirectSpeakersMatrixRoute {
    std::string source_label;
    std::string source_key;
    bool mute{false};
    std::vector<ResolvedDirectSpeakersMatrixTarget> targets;
};

struct ResolvedDirectSpeakersMatrix {
    std::vector<ResolvedDirectSpeakersMatrixRoute> routes;
    std::shared_ptr<MradmRenderMatrix> handle; // the Rust resolution `routes` copies
};

// Resolve every matrix target label to a concrete output slot. Unknown labels,
// alias-colliding duplicates, LFE rows/targets, and duplicate source rows fail.
[[nodiscard]] Result<ResolvedDirectSpeakersMatrix>
resolve_direct_speakers_matrix_targets(const DirectSpeakersMatrix& matrix,
                                       std::span<const DirectSpeakerRoutingTarget> targets,
                                       std::string_view layout_id);

// Match one non-LFE ADM block to exactly one resolved source row.
[[nodiscard]] Result<const ResolvedDirectSpeakersMatrixRoute*>
direct_speakers_matrix_route_for_block(const ResolvedDirectSpeakersMatrix& matrix,
                                       const SceneDirectSpeakersBlock& block);

struct DirectSpeakerPosition {
    float azimuth{0.0F};
    float elevation{0.0F};
};

// Recover a nominal direction from a known BS.2051 label or one of the shared
// RoomCentric/DAW aliases. This is used when label routing cannot find the same
// slot in the target layout (for example M+135 rendered to a 5.1 M+110 layout).
// Middle/upper/bottom/top layers use nominal elevations 0/+30/-30/+90 degrees.
[[nodiscard]] std::optional<DirectSpeakerPosition>
direct_speaker_position_for_labels(const std::vector<std::string>& labels);

// Read a DirectSpeakers block's nominal coordinates. Missing coordinates use
// front-centre (0, 0) and emit the shared warning required by both backends.
[[nodiscard]] DirectSpeakerPosition
direct_speaker_position_or_front(const SceneDirectSpeakersBlock& block, LogSink& logs, std::string_view log_module);

[[nodiscard]] std::vector<MradmRenderOverride> override_records(const LiveOverrides& overrides);
// Borrowed labels: rebuild after changing the owning LiveOverrides. Reuse across render queries.
struct LiveOverrideView {
    // cppcheck-suppress noExplicitConstructor
    // NOLINTNEXTLINE(google-explicit-constructor): control callers may pass a temporary view.
    LiveOverrideView(const LiveOverrides& overrides) : records(override_records(overrides)) {}
    std::vector<MradmRenderOverride> records;
};

// Resolve the live linear gain multiplier for one input channel given its owning object id and
// its canonicalised DirectSpeakers speaker label (empty for Objects / HOA channels). A channel-
// specific override (non-empty speaker_label matching channel_label_key) wins over a whole-object
// override (empty speaker_label); mute resolves to an exact 0 multiplier, and no matching override
// returns nullopt.
[[nodiscard]] std::optional<float> resolve_live_channel_gain(const LiveOverrideView& overrides,
                                                             std::string_view object_id,
                                                             std::string_view channel_label_key);

// Resolve an explicitly supplied live head-lock value. Channel-specific values
// win over whole-object values; entries whose head_locked is nullopt do not
// participate, so a gain-only channel override cannot shadow a whole-object
// head-lock override. nullopt means inherit the active ADM block.
[[nodiscard]] std::optional<bool> resolve_live_head_locked(const LiveOverrideView& overrides,
                                                           std::string_view object_id,
                                                           std::string_view channel_label_key);

// Sample-domain live-gain de-zipper. A target supplied before the first sample takes effect
// immediately (important when a monitor starts with existing edits); later targets traverse a
// short linear ramp and can be retargeted mid-ramp without a value discontinuity.
class LiveGainRamp {
  public:
    static constexpr uint32_t k_default_ramp_ms = 20U;

    explicit LiveGainRamp(uint32_t sample_rate, uint32_t ramp_ms = k_default_ramp_ms);

    // Restore an unstarted ramp; reapply the current target before processing a new timeline position.
    void reset() noexcept;
    void set_target(float target) noexcept;
    [[nodiscard]] float next() noexcept;
    void fill(std::span<float> output) noexcept;

  private:
    dsp::GainBank bank_;
};

// Applies one LiveGainRamp per channel to interleaved PCM. Render streams use this before their
// linear spatial mix, so gain automation is smooth without altering unrelated objects/channels.
class InterleavedLiveGainSmoother {
  public:
    InterleavedLiveGainSmoother(std::size_t channels, uint32_t sample_rate);

    void set_targets(std::span<const float> targets) noexcept;
    void apply(float* interleaved, std::size_t frames) noexcept;
    void fill(std::span<float> output) noexcept;
    void reset() noexcept;

  private:
    std::size_t channels_;
    dsp::GainBank bank_;
};

// ADM producers are inconsistent: some LFE DirectSpeakers channels carry
// channelFrequency lowPass, while others only encode the role in labels like
// RC_LFE/RCLFE/LFE1. Use this helper before positional fallback/spatialization.
[[nodiscard]] bool is_lfe_label(std::string_view raw) noexcept;
[[nodiscard]] bool direct_speakers_block_is_lfe(const SceneDirectSpeakersBlock& block) noexcept;

// Semantic destination encoded by one DirectSpeakers LFE block. LFE2/LFER are
// the second 22.2 LFE; all other recognised LFE aliases (including lowPass-only
// blocks) are the first. This classification is metadata-only.
enum class LfeTarget {
    none,
    lfe1,
    lfe2,
};

[[nodiscard]] LfeTarget direct_speakers_lfe_target(const SceneDirectSpeakersBlock& block) noexcept;

inline constexpr std::size_t k_22_2_lfe1_index = 3U;
inline constexpr std::size_t k_22_2_lfe2_index = 9U;
inline constexpr float k_lfe_split_power_gain = 0.70710678118654752440F;

[[nodiscard]] constexpr float mix_lfe_pair(float first, float second, bool both_active) noexcept {
    return (first + second) * (both_active ? k_lfe_split_power_gain : 1.0F);
}

// Shared, immutable 22.2 routing decision used by every speaker backend. The
// resolver scans scene metadata before any output writer is opened, so an invalid
// dual-LFE split request fails consistently in prepare().
struct LfeRoutingPlan {
    bool applies_to_22_2{false};
    LfeRoutingMode mode{LfeRoutingMode::direct};
    bool has_lfe1{false};
    bool has_lfe2{false};

    [[nodiscard]] float gain(LfeTarget input, LfeTarget output) const noexcept;
};

[[nodiscard]] Result<LfeRoutingPlan>
resolve_lfe_routing(const RenderPlan& plan, LogSink& logs, std::string_view log_module);

// Portion of an output block that should be fed to the loudness / True-Peak meter.
struct MeterChunk {
    std::size_t offset_frames{0}; // frames from the block start
    std::size_t frame_count{0};   // 0 = block lies entirely outside the window (skip)
};

// Intersect an output block [block_start, block_start + block_len) with an optional
// meter window (see RenderPlan::meter_window). A nullopt window measures the whole
// block. Used so trimmed output reports the loudness / peak of the kept segment
// while the backend still renders and writes the full timeline.
[[nodiscard]] inline MeterChunk
meter_window_chunk(const std::optional<MeterWindow>& window, uint64_t block_start, uint64_t block_len) {
    if (!window) {
        return {0, static_cast<std::size_t>(block_len)};
    }
    const uint64_t w_end = window->start_frame + window->frame_count;
    const uint64_t b_end = block_start + block_len;
    const uint64_t lo = std::max(block_start, window->start_frame);
    const uint64_t hi = std::min(b_end, w_end);
    if (hi <= lo) {
        return {0, 0};
    }
    return {static_cast<std::size_t>(lo - block_start), static_cast<std::size_t>(hi - lo)};
}

// Single background thread that runs posted tasks strictly in FIFO order. Renderers use it to move
// the loudness / true-peak measurement (libebur128) off the critical path so it overlaps with the
// next block's read + mix. Because tasks run in submission order on one thread, the sequence of
// ebur128 calls — and therefore the measured loudness / true peak — is bit-identical to running them
// inline; only the audio output buffer must be double-buffered so the next block does not overwrite
// a buffer still being measured (wait on the returned future before reuse).
class SerialWorker {
  public:
    SerialWorker();
    ~SerialWorker();
    SerialWorker(const SerialWorker&) = delete;
    SerialWorker& operator=(const SerialWorker&) = delete;
    SerialWorker(SerialWorker&&) = delete;
    SerialWorker& operator=(SerialWorker&&) = delete;

    // Enqueue a task; the returned future becomes ready once the task has run.
    std::future<void> post(std::function<void()> task);

  private:
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::packaged_task<void()>> tasks_;
    bool stop_{false};
    std::thread thread_;
};

struct PreparedObjectBlock {
    std::vector<SceneObjectBlock> sources;
    uint64_t start_sample{0};
    uint64_t end_sample{0};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

[[nodiscard]] PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                                       const SceneObject& object,
                                                       const std::vector<SceneOutputSpeaker>& speakers,
                                                       LogSink& logs,
                                                       std::string_view log_module,
                                                       bool& screen_ref_warned);

// One direction of the 17-point "disk cloud" used to approximate ADM object extent
// (width/height/depth) as a set of coherent point sources. azimuth/elevation are in the
// project polar convention (azimuth +ve = left, BS.2051). weight is a linear gain weight: a
// linear partition of unity (the active directions' weights sum to 1) so the cloud preserves
// the source's total gain.
struct ExtentDirection {
    float azimuth{0.0F};
    float elevation{0.0F};
    float weight{1.0F}; // linear gain weight
};

// Expand an object block position + extent (width/height/depth, each 0..1) into the
// 17-point disk cloud. When the effective extent is ~0 a single centre direction
// (weight 1) is returned, so a point object stays a point. Otherwise the 16 ring
// directions are returned (inner/outer rings, weights summing to 1), mapped around the
// position via a tangent frame with tan() angular scaling. Pure; the radius mapping
// (width*60, height*45, depth*20, distance-based spread scaling) matches the binaural /
// HOA extent clouds.
[[nodiscard]] std::vector<ExtentDirection>
extent_disk_cloud(const SceneBlockPosition& position, float width, float height, float depth);

} // namespace mradm::render_common
