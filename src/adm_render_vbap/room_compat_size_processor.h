#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "adm/errors.h"

#include "room_222.h"
#include "room_compat_size_panner.h"

namespace mradm::room_compat {

// Exact recursive structure used by the size candidate. The measured FIR
// SizeFilterBank remains a separate research comparison.
class SizeDecorrelator final {
  public:
    using FilteredFrame = std::array<float, 4>;
    static constexpr std::size_t subblock_frames = 32;
    SizeDecorrelator();
    void reset() noexcept;
    void process(std::span<const float, subblock_frames> input,
                 std::span<FilteredFrame, subblock_frames> output) noexcept;

  private:
    void reset_dsp() noexcept;
    std::array<std::array<FilteredFrame, 346>, 4> delays_{};
    std::array<std::size_t, 4> indices_{};
    std::array<float, 96> input_delay_{};
    std::size_t input_index_{};
    float pre_gain_{1};
    float post_gain_{1};
    float slow_level_{};
    float fast_level_{};
    float prefilter_state_{};
    bool initial_{true};
    unsigned silent_blocks_{};
};

struct SizeEvent {
    uint64_t start_sample{};
    SizePosition position;
    float size{};
};

// One state per object/track, owned by a render session. push() appends complete
// control blocks; finish() emits the original-length final partial block. This
// lets callers use arbitrary input chunks without changing the signal timeline.
class SizeObjectProcessor final {
  public:
    [[nodiscard]] static Result<SizeObjectProcessor>
    create(std::span<const SizeEvent> events, std::string layout, uint32_t sample_rate);
    void reset() noexcept;
    [[nodiscard]] Result<void> push(std::span<const float> input, std::vector<float>& output);
    [[nodiscard]] Result<void> finish(std::vector<float>& output);
    [[nodiscard]] std::size_t channel_count() const noexcept { return channels_; }

  private:
    [[nodiscard]] Result<void> process_control(std::vector<float>& output, std::size_t valid_frames);
    [[nodiscard]] Result<void> process_extended_control(std::vector<float>& output, std::size_t valid_frames);
    std::vector<SizeEvent> events_;
    std::string layout_;
    std::size_t channels_{};
    std::size_t next_event_{};
    uint64_t control_start_{};
    std::array<float, 512> pending_{};
    std::size_t pending_frames_{};
    bool finished_{};
    bool first_{true};
    SizePosition position_;
    SizePosition target_position_;
    float size_{};
    float target_size_{};
    SizeMixGains previous_mix_;
    std::array<float, 24> previous_point_{};
    Room222Mix previous_extended_mix_;
    Room222Mix cached_extended_mix_;
    Room222Gains cached_extended_point_{};
    SizePosition cached_position_;
    float cached_size_{-1};
    bool previous_filter_active_{};
    bool older_filter_active_{};
    SizeDecorrelator decorrelator_;
};

} // namespace mradm::room_compat
