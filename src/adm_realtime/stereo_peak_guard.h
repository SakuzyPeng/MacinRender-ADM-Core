#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mradm::realtime {

// Device-bound stereo PCM only. The Scene renderer's float output remains
// unbounded for offline processing. All storage is allocated before playback.
// cppcheck-suppress-begin unusedStructMember
class StereoPeakGuard {
  public:
    static constexpr std::size_t k_pull_frames = 4096U;
    static constexpr float k_ceiling = 0.89125094F; // -1 dBFS sample peak, not a true-peak claim.

    explicit StereoPeakGuard(std::uint32_t sample_rate);
    void reset() noexcept;
    [[nodiscard]] std::size_t lookahead_frames() const noexcept;
    [[nodiscard]] std::size_t buffered_frames() const noexcept;
    [[nodiscard]] std::size_t writable_frames() const noexcept;
    [[nodiscard]] std::size_t readable_frames(bool ended) const noexcept;
    void push(std::span<const float> stereo) noexcept;
    [[nodiscard]] std::size_t pop(std::span<float> output, float volume, bool ended) noexcept;

  private:
    [[nodiscard]] float next_gain(float volume) noexcept;

    std::vector<float> samples_;
    std::vector<float> peaks_;
    std::vector<float> attack_weights_;
    std::size_t read_{0U};
    std::size_t size_{0U};
    float gain_{1.0F};
    float release_;
};
// cppcheck-suppress-end unusedStructMember

} // namespace mradm::realtime
