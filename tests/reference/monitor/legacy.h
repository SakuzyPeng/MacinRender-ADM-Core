// Frozen numerical reference from a54fdc3. Test-only; see provenance.json.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace monitor_legacy {
inline constexpr std::size_t k_max_level_channels = 64;
inline constexpr uint64_t k_crossfade_frames = 2048;
struct State {
    explicit State(uint32_t channels, uint32_t rate, bool realtime)
        : channels_(channels), pull_is_realtime_playback_(realtime), last_output_frame_(channels, 0.0F),
          seek_transition_anchor_(channels, 0.0F),
          seek_transition_total_frames_(std::max<std::size_t>(1U, (static_cast<std::size_t>(rate) * 10U) / 1000U)) {}
    uint32_t channels_;
    bool pull_is_realtime_playback_;
    uint64_t observed_seek_generation_{0};
    std::vector<float> last_output_frame_;
    std::vector<float> seek_transition_anchor_;
    std::size_t seek_transition_total_frames_;
    std::size_t seek_transition_remaining_frames_{0};
    uint64_t xfade_pos_{0};
    void apply_seek_transition(
        std::span<float> out, std::size_t frames, std::size_t produced_frames, bool active, uint64_t generation) {
        if (frames == 0 || channels_ == 0) {
            return;
        }

        const std::size_t real_frames = std::min(frames, produced_frames);
        if (!active) {
            // Paused/flushing callback output is exact silence. Keep the generation unobserved so the
            // first real post-seek samples still receive a fade-in when playback resumes.
            std::ranges::fill(last_output_frame_, 0.0F);
            seek_transition_remaining_frames_ = 0;
            return;
        }

        if (real_frames > 0 && generation != observed_seek_generation_) {
            observed_seek_generation_ = generation;
            seek_transition_remaining_frames_ = seek_transition_total_frames_;
            if (pull_is_realtime_playback_) {
                seek_transition_anchor_ = last_output_frame_;
            } else {
                // A push sink discards/mutes its old system queue on seek, so its new queue starts from
                // silence rather than from the last frame that happened to be enqueued far ahead.
                std::ranges::fill(seek_transition_anchor_, 0.0F);
            }
        }

        for (std::size_t frame = 0; frame < real_frames && seek_transition_remaining_frames_ > 0; ++frame) {
            const std::size_t elapsed = seek_transition_total_frames_ - seek_transition_remaining_frames_;
            const float mix =
                seek_transition_total_frames_ <= 1
                    ? 1.0F
                    : static_cast<float>(elapsed) / static_cast<float>(seek_transition_total_frames_ - 1U);
            for (std::size_t channel = 0; channel < channels_; ++channel) {
                const std::size_t index = (frame * channels_) + channel;
                out[index] = (seek_transition_anchor_[channel] * (1.0F - mix)) + (out[index] * mix);
            }
            --seek_transition_remaining_frames_;
        }

        // Realtime devices play the silence-padded tail of a short read; push devices enqueue only the
        // produced portion. Remember the last sample that actually reaches each kind of sink.
        const std::size_t emitted_frames = pull_is_realtime_playback_ ? frames : real_frames;
        if (emitted_frames > 0) {
            const std::size_t last = (emitted_frames - 1U) * channels_;
            std::copy_n(out.data() + last, channels_, last_output_frame_.data());
        }
        if (pull_is_realtime_playback_ && real_frames < frames) {
            // The device emitted a zero-padded underrun tail, so a partially completed bridge can no
            // longer continue from its old anchor without creating a second discontinuity.
            seek_transition_remaining_frames_ = 0;
        }
    }


    void levels(std::span<const float> out, std::size_t frames, std::span<float> peaks, std::span<float> rms) const {
        // Per-channel peak / RMS over the block (silence included), for the UI meters.
        const std::size_t meter_ch = std::min<std::size_t>(channels_, k_max_level_channels);
        for (std::size_t c = 0; c < meter_ch; ++c) {
            float peak = 0.0F;
            double sumsq = 0.0;
            for (std::size_t f = 0; f < frames; ++f) {
                const float v = out[(f * channels_) + c];
                peak = std::max(peak, std::fabs(v));
                sumsq += static_cast<double>(v) * static_cast<double>(v);
            }
            peaks[c] = peak;
            rms[c] = frames > 0 ? static_cast<float>(std::sqrt(sumsq / static_cast<double>(frames))) : 0.0F;
        }
    }
    bool crossfade(std::span<float> out, std::span<const float> incoming, std::size_t got) {
        for (std::size_t f = 0; f < got; ++f) {
            const auto p = static_cast<double>(xfade_pos_ + f);
            const float t = static_cast<float>(std::min(1.0, p / static_cast<double>(k_crossfade_frames)));
            for (uint32_t c = 0; c < channels_; ++c) {
                const std::size_t i = (f * channels_) + c;
                out[i] = (out[i] * (1.0F - t)) + (incoming[i] * t);
            }
        }
        xfade_pos_ += got;
        return xfade_pos_ >= k_crossfade_frames;
    }
};
} // namespace monitor_legacy
