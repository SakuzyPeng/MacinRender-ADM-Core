#pragma once
// Frozen scalar loops from 726db31. Thread/queue scheduling is tested through the public API.
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
namespace scene_numeric_reference {
struct Transitions {
    size_t channels;uint32_t rate;
    uint64_t backend_crossfade_position=0,transition_remaining=0,transition_position=0;
    std::vector<float> last_output_frame,transition_anchor;
    static constexpr uint64_t k_backend_crossfade_frames=2048;
    Transitions(size_t channels,uint32_t rate):channels(channels),rate(rate),last_output_frame(channels),transition_anchor(channels){}
    void begin_generation(){transition_anchor=last_output_frame;transition_remaining=std::max<uint64_t>(1,uint64_t(rate)*10/1000);transition_position=0;}
    bool mix(std::span<float>render_output,std::span<const float>render_output_b,uint32_t frames){
        for (std::uint32_t frame_index = 0U; frame_index < frames; ++frame_index) {
            const auto position = std::min<std::uint64_t>(backend_crossfade_position + 1U, k_backend_crossfade_frames);
            const float incoming_weight = static_cast<float>(position) / static_cast<float>(k_backend_crossfade_frames);
            const float outgoing_weight = 1.0F - incoming_weight;
            const auto output_index = static_cast<std::size_t>(frame_index) * channels;
            for (std::size_t channel = 0U; channel < channels; ++channel) {
                render_output[output_index + channel] = (render_output[output_index + channel] * outgoing_weight) +
                                                        (render_output_b[output_index + channel] * incoming_weight);
            }
            ++backend_crossfade_position;
        }
        return backend_crossfade_position>=k_backend_crossfade_frames;
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
};
}
