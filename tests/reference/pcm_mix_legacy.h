// Frozen test-only arithmetic from 4286f3e; never linked into production.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pcm_mix_legacy {
template <typename Channel>
[[nodiscard]] uint64_t block_active_length(const Channel& channel, std::size_t block_index) {
    const auto& block = channel.blocks[block_index];
    uint64_t active_end = block.end_sample;
    if (block_index + 1 < channel.blocks.size()) {
        active_end = std::min(active_end, channel.blocks[block_index + 1].start_sample);
    }
    if (active_end <= block.start_sample) {
        return 0;
    }
    return active_end - block.start_sample;
}

template <typename Channel>
[[nodiscard]] uint64_t interpolation_length(const Channel& channel, std::size_t block_index, uint64_t default_interp) {
    const auto& block = channel.blocks[block_index];
    if (block.jump_position || block_index == 0) {
        return 0;
    }
    return std::min(block.interp_length_samples.value_or(default_interp), block_active_length(channel, block_index));
}

template <typename T> [[nodiscard]] T interpolated_scalar(T previous, T current, uint64_t delta, uint64_t interp_len) {
    const auto alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
    return static_cast<T>((static_cast<double>(previous) * (1.0 - alpha)) + (static_cast<double>(current) * alpha));
}

namespace common {
struct BlockGains {
    std::vector<float> gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    bool smoothable_object{false};
    std::optional<uint64_t> interp_length_samples;
};

// One input channel with its full sorted block sequence.
struct ChannelGainInfo {
    uint16_t input_channel{0};
    // cppcheck-suppress unusedStructMember
    std::string object_id; // owning SceneObject::id, for live gain overrides
    // cppcheck-suppress unusedStructMember
    std::string speaker_label_key;  // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
    std::vector<BlockGains> blocks; // sorted by start_sample
    float output_gain{1.0F};
};

struct AccumulateContext {
    const float* input{nullptr};
    std::vector<float>* output{nullptr};
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
    uint64_t object_smoothing_frames{0};
    // Optional sample-domain output gain per input channel, after spatial/user gain.
    std::span<const float> live_gains{};
};

namespace {
void accumulate_channel_block(const ChannelGainInfo& channel,
                              std::size_t& block_index,
                              const AccumulateContext& ctx,
                              std::size_t frame) {
    const uint64_t abs_frame = ctx.frames_done + frame;
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return;
    }

    const float in_sample = ctx.input[(frame * ctx.num_in_ch) + channel.input_channel];
    const uint64_t interp_len = interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;

    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        float gain = block.gains[out_ch];
        if (ramping) {
            gain = interpolated_scalar(
                channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len);
        }
        float contribution = (in_sample * gain) * channel.output_gain;
        if (!ctx.live_gains.empty()) {
            contribution *= ctx.live_gains[(frame * ctx.num_in_ch) + channel.input_channel];
        }
        (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += contribution;
    }
}

[[nodiscard]] bool gains_at_frame(const ChannelGainInfo& channel,
                                  std::size_t& block_index,
                                  const AccumulateContext& ctx,
                                  uint64_t abs_frame,
                                  std::vector<float>& gains) {
    std::ranges::fill(gains, 0.0F);
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return false;
    }

    const uint64_t interp_len = interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;
    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        gains[out_ch] = block.gains[out_ch];
        if (ramping) {
            gains[out_ch] = interpolated_scalar(
                channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len);
        }
    }
    return block.smoothable_object;
}

} // namespace

void accumulate_speaker_channel(const ChannelGainInfo& channel,
                                std::size_t& block_index,
                                const AccumulateContext& ctx,
                                uint64_t frames_now) {
    if (channel.blocks.empty()) {
        return;
    }
    for (std::size_t frame = 0; frame < frames_now; ++frame) {
        accumulate_channel_block(channel, block_index, ctx, frame);
    }
}

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now) {
    std::vector<float> start_gains(ctx.num_out_ch);
    std::vector<float> end_gains(ctx.num_out_ch);
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& channel = gain_matrix[ci];
        if (channel.blocks.empty()) {
            continue;
        }
        auto start_index = block_indices[ci];
        auto end_index = start_index;
        const bool smooth_start =
            ctx.object_smoothing_frames > 0 && gains_at_frame(channel, start_index, ctx, ctx.frames_done, start_gains);
        const bool smooth_end = ctx.object_smoothing_frames > 0 &&
                                gains_at_frame(channel, end_index, ctx, ctx.frames_done + frames_now - 1, end_gains);
        if (!smooth_start || !smooth_end) {
            for (std::size_t frame = 0; frame < frames_now; ++frame) {
                accumulate_channel_block(channel, block_indices[ci], ctx, frame);
            }
            continue;
        }
        block_indices[ci] = start_index;

        for (std::size_t frame = 0; frame < frames_now; ++frame) {
            const float alpha = frames_now > 1 ? static_cast<float>(frame) / static_cast<float>(frames_now - 1) : 0.0F;
            const float in_sample = ctx.input[(frame * ctx.num_in_ch) + channel.input_channel];
            for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
                const float gain = (start_gains[out_ch] * (1.0F - alpha)) + (end_gains[out_ch] * alpha);
                float contribution = (in_sample * gain) * channel.output_gain;
                if (!ctx.live_gains.empty()) {
                    contribution *= ctx.live_gains[(frame * ctx.num_in_ch) + channel.input_channel];
                }
                (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += contribution;
            }
        }
    }
}

} // namespace common
namespace ear {
struct BlockGains {
    std::vector<double> gains;
    std::vector<double> diffuse_gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    bool smoothable_object{false};
    std::optional<uint64_t> interp_length_samples;
};

struct ChannelGainInfo {
    uint16_t input_channel{0};
    // cppcheck-suppress unusedStructMember
    std::string object_id; // owning SceneObject::id (empty for HOA-pack tracks); live gain key
    // cppcheck-suppress unusedStructMember
    std::string speaker_label_key;  // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
    std::vector<BlockGains> blocks; // sorted by start_sample
};

struct AccumulateContext {
    const float* input{nullptr};
    float* col_direct{nullptr};  // [num_out_ch × frames_cap] column-major, float
    float* col_diffuse{nullptr}; // [num_out_ch × frames_cap] column-major, float
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
    uint64_t object_smoothing_frames{0};
    uint64_t frames_cap{0}; // stride between columns (= k_block_size)
};

void accumulate_channel_segment(const ChannelGainInfo& channel,
                                std::size_t block_index,
                                const AccumulateContext& ctx,
                                const float* ch_in,
                                std::size_t f0,
                                std::size_t f1) {
    const auto& block = channel.blocks[block_index];
    const uint64_t abs_start = ctx.frames_done;

    const uint64_t interp_len = interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta0 = (abs_start + f0) - block.start_sample;
    const bool any_ramp = interp_len > 0 && delta0 < interp_len;

    const std::size_t num_out = ctx.num_out_ch;

    const uint64_t stride = ctx.frames_cap;

    if (!any_ramp) {
        // Fast path: gains constant over this window → saxpy per output channel.
        for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
            const auto gd = static_cast<float>(block.gains[out_ch]);
            const auto gf = static_cast<float>(block.diffuse_gains[out_ch]);
            if (gd == 0.0F && gf == 0.0F) {
                continue; // skip sparse zeros (common for VBAP panning)
            }
            float* col_d = ctx.col_direct + (out_ch * stride);
            float* col_f = ctx.col_diffuse + (out_ch * stride);
            if (gd != 0.0F) {
                for (std::size_t f = f0; f < f1; ++f) {
                    col_d[f] += ch_in[f] * gd;
                }
            }
            if (gf != 0.0F) {
                for (std::size_t f = f0; f < f1; ++f) {
                    col_f[f] += ch_in[f] * gf;
                }
            }
        }
    } else {
        // Slow path: interpolating — per-frame scalar fallback.
        for (std::size_t f = f0; f < f1; ++f) {
            const uint64_t delta = (abs_start + f) - block.start_sample;
            const bool ramping = delta < interp_len;
            const float in = ch_in[f];
            for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
                const auto gd = static_cast<float>(
                    ramping ? interpolated_scalar(
                                  channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len)
                            : block.gains[out_ch]);
                const auto gf = static_cast<float>(
                    ramping ? interpolated_scalar(channel.blocks[block_index - 1].diffuse_gains[out_ch],
                                                  block.diffuse_gains[out_ch],
                                                  delta,
                                                  interp_len)
                            : block.diffuse_gains[out_ch]);
                ctx.col_direct[(out_ch * stride) + f] += in * gd;
                ctx.col_diffuse[(out_ch * stride) + f] += in * gf;
            }
        }
    }
}

// Accumulate one input channel into the column-major direct/diffuse buffers.
// Fast path (static gains): inner loop is a plain saxpy -> auto-vectorised.
// Slow path (ramping): per-frame scalar fallback.
void accumulate_channel_block(const ChannelGainInfo& channel,
                              std::size_t& block_index,
                              const AccumulateContext& ctx,
                              const float* ch_in, // deinterleaved, [frames_now]
                              uint64_t frames_now) {
    const uint64_t abs_start = ctx.frames_done;
    const uint64_t win_end = abs_start + frames_now;
    std::size_t f0 = 0;

    while (f0 < frames_now) {
        const uint64_t abs_frame = abs_start + f0;
        while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
            ++block_index;
        }

        const auto& block = channel.blocks[block_index];
        if (abs_frame < block.start_sample) {
            f0 = static_cast<std::size_t>(std::min(block.start_sample - abs_start, win_end - abs_start));
            continue;
        }
        if (abs_frame >= block.end_sample) {
            if (block_index + 1 >= channel.blocks.size()) {
                return;
            }
            const uint64_t next_start = channel.blocks[block_index + 1].start_sample;
            f0 = static_cast<std::size_t>(std::min(std::max(abs_frame, next_start) - abs_start, frames_now));
            ++block_index;
            continue;
        }

        uint64_t segment_end = std::min(block.end_sample, win_end);
        if (block_index + 1 < channel.blocks.size()) {
            segment_end = std::min(segment_end, channel.blocks[block_index + 1].start_sample);
        }
        if (segment_end <= abs_frame) {
            ++block_index;
            continue;
        }

        const auto f1 = static_cast<std::size_t>(segment_end - abs_start);
        accumulate_channel_segment(channel, block_index, ctx, ch_in, f0, f1);
        f0 = f1;
    }
}

[[nodiscard]] bool gains_at_frame(const ChannelGainInfo& channel,
                                  std::size_t& block_index,
                                  const AccumulateContext& ctx,
                                  uint64_t abs_frame,
                                  std::vector<float>& direct,
                                  std::vector<float>& diffuse) {
    std::ranges::fill(direct, 0.0F);
    std::ranges::fill(diffuse, 0.0F);
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return false;
    }

    const uint64_t interp_len = interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;
    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        direct[out_ch] = static_cast<float>(block.gains[out_ch]);
        diffuse[out_ch] = static_cast<float>(block.diffuse_gains[out_ch]);
        if (ramping) {
            direct[out_ch] = interpolated_scalar(
                static_cast<float>(channel.blocks[block_index - 1].gains[out_ch]), direct[out_ch], delta, interp_len);
            diffuse[out_ch] =
                interpolated_scalar(static_cast<float>(channel.blocks[block_index - 1].diffuse_gains[out_ch]),
                                    diffuse[out_ch],
                                    delta,
                                    interp_len);
        }
    }
    return block.smoothable_object;
}

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now,
                            std::vector<float>& ch_in_buf) {
    std::vector<float> start_direct(ctx.num_out_ch);
    std::vector<float> end_direct(ctx.num_out_ch);
    std::vector<float> start_diffuse(ctx.num_out_ch);
    std::vector<float> end_diffuse(ctx.num_out_ch);
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& channel = gain_matrix[ci];
        if (channel.blocks.empty()) {
            continue;
        }
        // Deinterleave this input channel into a contiguous buffer.
        const uint16_t ic = channel.input_channel;
        const uint16_t num_in = ctx.num_in_ch;
        for (std::size_t f = 0; f < frames_now; ++f) {
            ch_in_buf[f] = ctx.input[(f * num_in) + ic];
        }
        auto start_index = block_indices[ci];
        auto end_index = start_index;
        const bool smooth_start =
            ctx.object_smoothing_frames > 0 &&
            gains_at_frame(channel, start_index, ctx, ctx.frames_done, start_direct, start_diffuse);
        const bool smooth_end =
            ctx.object_smoothing_frames > 0 &&
            gains_at_frame(channel, end_index, ctx, ctx.frames_done + frames_now - 1, end_direct, end_diffuse);
        if (!smooth_start || !smooth_end) {
            accumulate_channel_block(channel, block_indices[ci], ctx, ch_in_buf.data(), frames_now);
            continue;
        }
        block_indices[ci] = start_index;
        const uint64_t stride = ctx.frames_cap;
        for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
            float* col_d = ctx.col_direct + (out_ch * stride);
            float* col_f = ctx.col_diffuse + (out_ch * stride);
            for (std::size_t f = 0; f < frames_now; ++f) {
                const float alpha = frames_now > 1 ? static_cast<float>(f) / static_cast<float>(frames_now - 1) : 0.0F;
                const float gd = (start_direct[out_ch] * (1.0F - alpha)) + (end_direct[out_ch] * alpha);
                const float gf = (start_diffuse[out_ch] * (1.0F - alpha)) + (end_diffuse[out_ch] * alpha);
                col_d[f] += ch_in_buf[f] * gd;
                col_f[f] += ch_in_buf[f] * gf;
            }
        }
    }
}

} // namespace ear
} // namespace pcm_mix_legacy

namespace pcm_mix_legacy {
inline void matrix(std::span<const float> scratch_,
                   std::span<float> out,
                   std::span<const float> matrix_,
                   uint32_t src_channels_,
                   uint32_t monitor_channels_,
                   std::size_t got) {
    for (std::size_t f = 0; f < got; ++f) {
        const float* in = scratch_.data() + (f * src_channels_);
        float* dst = out.data() + (f * monitor_channels_);
        for (uint32_t d = 0; d < monitor_channels_; ++d) {
            const float* row = matrix_.data() + (static_cast<std::size_t>(d) * src_channels_);
            float acc = 0.0F;
            for (uint32_t s = 0; s < src_channels_; ++s) {
                acc += row[s] * in[s];
            }
            dst[d] = acc;
        }
    }
}
} // namespace pcm_mix_legacy
