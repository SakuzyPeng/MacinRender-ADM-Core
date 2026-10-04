#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mradm::realtime {

// Device-bound stereo PCM only. The Scene renderer's float output remains
// unbounded for offline processing. All storage is allocated before playback.
// cppcheck-suppress-begin unusedStructMember
class StereoPeakGuard {
  public:
    static constexpr std::size_t k_pull_frames = 4096U;
    static constexpr float k_ceiling = 0.89125094F; // -1 dBFS sample peak, not a true-peak claim.

    explicit StereoPeakGuard(std::uint32_t sample_rate);
    ~StereoPeakGuard();
    StereoPeakGuard(const StereoPeakGuard&) = delete;
    StereoPeakGuard& operator=(const StereoPeakGuard&) = delete;
    StereoPeakGuard(StereoPeakGuard&& other) noexcept;
    StereoPeakGuard& operator=(StereoPeakGuard&& other) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t lookahead_frames() const noexcept;
    [[nodiscard]] std::size_t buffered_frames() const noexcept;
    [[nodiscard]] std::size_t writable_frames() const noexcept;
    [[nodiscard]] std::size_t readable_frames(bool ended) const noexcept;
    void push(std::span<const float> stereo) noexcept;
    [[nodiscard]] std::size_t pop(std::span<float> output, float volume, bool ended) noexcept;

  private:
    void* handle_{nullptr};
};
// cppcheck-suppress-end unusedStructMember

} // namespace mradm::realtime
