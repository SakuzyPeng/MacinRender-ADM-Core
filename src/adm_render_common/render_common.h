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

#include "../adm_dsp/gain.h"
#include "routing_ffi.h"

namespace mradm::render_common {

// One output of a resolved DirectSpeakers matrix route.
struct ResolvedDirectSpeakersMatrixTarget {
    std::size_t output_channel{0};
    float gain{0.0F};
};

[[nodiscard]] std::vector<MradmRenderOverride> override_records(const LiveOverrides& overrides);
// The same records with the topology scales the file streams validate or apply.
[[nodiscard]] std::vector<MradmRenderLiveOverride> live_override_records(const LiveOverrides& overrides);
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

// Semantic destination encoded by one DirectSpeakers LFE block. LFE2/LFER are
// the second 22.2 LFE; all other recognised LFE aliases (including lowPass-only
// blocks) are the first. This classification is metadata-only.
enum class LfeTarget {
    none,
    lfe1,
    lfe2,
};

inline constexpr std::size_t k_22_2_lfe1_index = 3U;
inline constexpr std::size_t k_22_2_lfe2_index = 9U;
inline constexpr float k_lfe_split_power_gain = 0.70710678118654752440F;

[[nodiscard]] constexpr float mix_lfe_pair(float first, float second, bool both_active) noexcept {
    return (first + second) * (both_active ? k_lfe_split_power_gain : 1.0F);
}

// The 22.2 LFE routing decision a backend prepared (Rust mradm-render `lfe_routing`).
struct LfeRoutingPlan {
    bool applies_to_22_2{false};
    LfeRoutingMode mode{LfeRoutingMode::direct};
    bool has_lfe1{false};
    bool has_lfe2{false};

    [[nodiscard]] float gain(LfeTarget input, LfeTarget output) const noexcept;
};

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

} // namespace mradm::render_common
