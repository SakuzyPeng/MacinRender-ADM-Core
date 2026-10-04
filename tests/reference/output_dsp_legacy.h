// Test-only C++ reference from 53cc36e. Keep arithmetic and state order frozen.
#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace output_dsp_legacy {


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

class LiveGainRamp {
  public:
    static constexpr uint32_t k_default_ramp_ms = 20U;

    explicit LiveGainRamp(uint32_t sample_rate, uint32_t ramp_ms = k_default_ramp_ms) noexcept;

    void set_target(float target) noexcept;
    [[nodiscard]] float next() noexcept;

  private:
    std::size_t ramp_frames_{1U};
    std::size_t remaining_frames_{0U};
    float current_{1.0F};
    float target_{1.0F};
    float step_{0.0F};
    bool started_{false};
};

// Applies one LiveGainRamp per channel to interleaved PCM. Render streams use this before their
// linear spatial mix, so gain automation is smooth without altering unrelated objects/channels.
class InterleavedLiveGainSmoother {
  public:
    InterleavedLiveGainSmoother(std::size_t channels, uint32_t sample_rate);

    void set_targets(std::span<const float> targets) noexcept;
    void apply(float* interleaved, std::size_t frames) noexcept;

  private:
    std::vector<LiveGainRamp> ramps_;
};


StereoPeakGuard::StereoPeakGuard(std::uint32_t sample_rate)
    : attack_weights_(std::max(1U, sample_rate / 200U) + 1U),
      release_(1.0F - std::exp(-1.0F / (0.1F * static_cast<float>(sample_rate)))) {
    const auto capacity = k_pull_frames + lookahead_frames();
    samples_.resize(capacity * 2U);
    peaks_.resize(capacity);
    for (std::size_t index = 0U; index < attack_weights_.size(); ++index) {
        attack_weights_[index] = 1.0F - (static_cast<float>(index) / static_cast<float>(lookahead_frames()));
    }
}

void StereoPeakGuard::reset() noexcept {
    read_ = 0U;
    size_ = 0U;
    gain_ = 1.0F;
}

std::size_t StereoPeakGuard::lookahead_frames() const noexcept {
    return attack_weights_.size() - 1U;
}
std::size_t StereoPeakGuard::buffered_frames() const noexcept {
    return size_;
}
std::size_t StereoPeakGuard::writable_frames() const noexcept {
    return peaks_.size() - size_;
}
std::size_t StereoPeakGuard::readable_frames(bool ended) const noexcept {
    return ended ? size_ : size_ - std::min(size_, lookahead_frames());
}

void StereoPeakGuard::push(std::span<const float> stereo) noexcept {
    assert(stereo.size() % 2U == 0U && stereo.size() / 2U <= writable_frames());
    auto write = (read_ + size_) % peaks_.size();
    for (std::size_t index = 0U; index < stereo.size(); index += 2U) {
        const float left = std::isfinite(stereo[index]) ? stereo[index] : 0.0F;
        const float right = std::isfinite(stereo[index + 1U]) ? stereo[index + 1U] : 0.0F;
        samples_[write * 2U] = left;
        samples_[(write * 2U) + 1U] = right;
        peaks_[write] = std::max(std::abs(left), std::abs(right));
        write = (write + 1U) % peaks_.size();
    }
    size_ += stereo.size() / 2U;
}

float StereoPeakGuard::next_gain(float volume) noexcept {
    float gain = gain_ + ((1.0F - gain_) * release_);
    const auto count = std::min(size_, attack_weights_.size());
    auto sample = read_;
    for (std::size_t ahead = 0U; ahead < count; ++ahead) {
        const float peak = peaks_[sample] * volume;
        if (peak > k_ceiling) {
            const float required = k_ceiling / peak;
            // Each future peak imposes a linear attack ending at its sample.
            // The minimum of these ramps is continuous and reaches the ceiling
            // in time, unlike an instantaneous block-peak gain change.
            gain = std::min(gain, 1.0F - ((1.0F - required) * attack_weights_[ahead]));
        }
        ++sample;
        if (sample == peaks_.size()) {
            sample = 0U;
        }
    }
    gain_ = gain;
    return gain;
}

std::size_t StereoPeakGuard::pop(std::span<float> output, float volume, bool ended) noexcept {
    const auto frames = std::min(output.size() / 2U, readable_frames(ended));
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const float gain = volume * next_gain(volume);
        output[frame * 2U] = samples_[read_ * 2U] * gain;
        output[(frame * 2U) + 1U] = samples_[(read_ * 2U) + 1U] * gain;
        read_ = (read_ + 1U) % peaks_.size();
        --size_;
    }
    return frames;
}

LiveGainRamp::LiveGainRamp(uint32_t sample_rate, uint32_t ramp_ms) noexcept
    : ramp_frames_(std::max<std::size_t>(
          1U, (static_cast<std::size_t>(sample_rate) * static_cast<std::size_t>(ramp_ms)) / 1000U)) {}

void LiveGainRamp::set_target(float target) noexcept {
    if (target == target_) {
        return;
    }
    target_ = target;
    if (!started_) {
        return;
    }
    if (ramp_frames_ <= 1U || current_ == target_) {
        current_ = target_;
        remaining_frames_ = 0U;
        step_ = 0.0F;
        return;
    }
    remaining_frames_ = ramp_frames_;
    step_ = (target_ - current_) / static_cast<float>(ramp_frames_ - 1U);
}

float LiveGainRamp::next() noexcept {
    if (!started_) {
        started_ = true;
        current_ = target_;
        remaining_frames_ = 0U;
        return current_;
    }

    const float value = current_;
    if (remaining_frames_ > 1U) {
        current_ += step_;
        --remaining_frames_;
    } else if (remaining_frames_ == 1U) {
        current_ = target_;
        remaining_frames_ = 0U;
        step_ = 0.0F;
    }
    return value;
}

InterleavedLiveGainSmoother::InterleavedLiveGainSmoother(std::size_t channels, uint32_t sample_rate) {
    ramps_.reserve(channels);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        ramps_.emplace_back(sample_rate);
    }
}

void InterleavedLiveGainSmoother::set_targets(std::span<const float> targets) noexcept {
    const std::size_t count = std::min(ramps_.size(), targets.size());
    for (std::size_t channel = 0; channel < count; ++channel) {
        ramps_[channel].set_target(targets[channel]);
    }
    for (std::size_t channel = count; channel < ramps_.size(); ++channel) {
        ramps_[channel].set_target(1.0F);
    }
}

void InterleavedLiveGainSmoother::apply(float* interleaved, std::size_t frames) noexcept {
    const std::size_t channels = ramps_.size();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            interleaved[(frame * channels) + channel] *= ramps_[channel].next();
        }
    }
}

} // namespace output_dsp_legacy
