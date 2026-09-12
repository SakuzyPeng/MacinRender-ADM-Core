#include "stereo_peak_guard.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace mradm::realtime {

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

} // namespace mradm::realtime
