#include "render_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "panner.h"

namespace mradm::triple_balance {

Result<SizeMixer> SizeMixer::create(const Prepared& prepared, const RenderPlan& plan, bool live_points) {
    SizeMixer result;
    result.prepared_ = &prepared;
    result.layout_ = plan.output_layout;
    result.input_channels_ = plan.scene.info.num_channels;
    result.default_interp_ = uint64_t{plan.scene.info.sample_rate} * plan.default_interp_ms / 1000;
    for (const auto& track : prepared.size_tracks) {
        auto processor = SizeObjectProcessor::create(track.events, plan.output_layout, plan.scene.info.sample_rate);
        if (!processor) {
            return tl::unexpected{processor.error()};
        }
        result.processors_.push_back(std::move(*processor));
        if (live_points) {
            auto gains = point_gains(track.initial_position, 1, plan.output_layout);
            if (!gains) {
                return tl::unexpected{gains.error()};
            }
            PointMotionState point;
            point.position = point.target = track.events.front().position;
            std::copy(gains->begin(), gains->end(), point.gains.begin());
            result.initial_points_.push_back(point);
            render_common::ChannelGainInfo channel;
            channel.input_channel = track.input_channel;
            channel.blocks.reserve(3);
            result.point_channels_.push_back(std::move(channel));
        }
    }
    result.points_ = result.initial_points_;
    result.weights_.assign(result.processors_.size(), 1.0F);
    result.targets_ = result.weights_;
    result.point_in_matrix_.assign(result.processors_.size(), false);
    result.input_.reserve(1024);
    result.output_.reserve(std::size_t{1024} * prepared.output_channels);
    result.point_output_.reserve(std::size_t{1024} * prepared.output_channels);
    return result;
}

void SizeMixer::set_scales(std::span<const float> scales, bool immediate) {
    for (std::size_t i = 0; i < processors_.size(); ++i) {
        processors_[i].set_size_scale(scales[i]);
        targets_[i] = scales[i] == 0 ? 0.0F : 1.0F;
        if (immediate) {
            weights_[i] = targets_[i];
        }
    }
}

void SizeMixer::reset() {
    for (auto& processor : processors_) {
        processor.reset();
    }
    weights_ = targets_;
    points_ = initial_points_;
}

void SizeMixer::set_point_in_matrix(std::span<const float> scales) {
    for (std::size_t i = 0; i < processors_.size(); ++i) {
        point_in_matrix_[i] = scales[i] == 0;
    }
}

std::vector<SizeTrackState> SizeMixer::snapshot() const {
    std::vector<SizeTrackState> result;
    result.reserve(processors_.size());
    for (std::size_t i = 0; i < processors_.size(); ++i) {
        result.push_back({processors_[i].snapshot(), points_.empty() ? PointMotionState{} : points_[i]});
    }
    return result;
}

void SizeMixer::restore(std::span<const SizeTrackState> states) {
    for (std::size_t i = 0; i < processors_.size(); ++i) {
        processors_[i].restore(states[i].size);
        if (!points_.empty()) {
            points_[i] = states[i].point;
        }
    }
    weights_ = targets_;
}

// Advance the same float-coordinate recurrence as triple_balance_motion_blocks, but
// retain only this block's gain changes. No curve grows with the file duration.
Result<void> SizeMixer::prepare_points(uint64_t start_frame, std::size_t frames) {
    const float alpha = 1.0F - std::exp(-512.0F / 1200.0F);
    for (std::size_t i = 0; i < points_.size(); ++i) {
        auto& state = points_[i];
        auto& channel = point_channels_[i];
        const auto& events = prepared_->size_tracks[i].events;
        if (state.control_start != start_frame) {
            return make_error(ErrorCode::internal_error, "Triple Balance point state lost block alignment");
        }
        channel.blocks.clear();
        channel.blocks.push_back(
            {std::vector<float>(state.gains.begin(), state.gains.begin() + prepared_->output_channels),
             0,
             std::numeric_limits<uint64_t>::max(),
             true,
             true,
             std::nullopt});
        for (std::size_t offset = 0; offset < frames; offset += 512) {
            // A single static event preserves its original coordinates, exactly as the
            // offline point fast path (including half-code quantization boundaries).
            if (state.control_start != 0 && events.size() > 1) {
                if (state.next_event < events.size() &&
                    events[state.next_event].start_sample < state.control_start + 512) {
                    state.target = events[state.next_event++].position;
                }
                state.position.x += alpha * (state.target.x - state.position.x);
                state.position.y += alpha * (state.target.y - state.position.y);
                state.position.z += alpha * (state.target.z - state.position.z);
                SceneBlockPosition position;
                position.cartesian = true;
                position.x = (state.position.x * 2) - 1;
                position.y = 1 - (state.position.y * 2);
                position.z = state.position.z;
                auto gains = point_gains(position, 1, layout_);
                if (!gains) {
                    return tl::unexpected{gains.error()};
                }
                if (!std::equal(gains->begin(), gains->end(), state.gains.begin())) {
                    std::copy(gains->begin(), gains->end(), state.gains.begin());
                    channel.blocks.push_back({std::move(*gains),
                                              state.control_start,
                                              std::numeric_limits<uint64_t>::max(),
                                              false,
                                              true,
                                              uint64_t{512}});
                }
            }
            state.control_start += 512;
        }
    }
    return {};
}

void SizeMixer::accumulate_point(std::size_t track,
                                 const render_common::AccumulateContext& context,
                                 std::size_t frames,
                                 bool user_gain) {
    auto& channel = point_channels_[track];
    channel.output_gain = user_gain ? prepared_->size_tracks[track].output_gain : 1.0F;
    std::size_t index = 0;
    render_common::accumulate_speaker_channel(channel, index, context, frames);
}

Result<void> SizeMixer::process(std::span<const float> source,
                                std::span<float> mixed,
                                uint64_t start_frame,
                                bool final,
                                std::span<const float> live_gains) {
    const auto frames = source.size() / input_channels_;
    const auto channels = prepared_->output_channels;
    input_.resize(frames);
    for (std::size_t i = 0; i < processors_.size(); ++i) {
        const auto& track = prepared_->size_tracks[i];
        for (std::size_t frame = 0; frame < frames; ++frame) {
            input_[frame] = source[(frame * input_channels_) + track.input_channel];
        }
        output_.clear();
        auto status = processors_[i].push(input_, output_);
        if (status && final) {
            status = processors_[i].finish(output_);
        }
        if (!status) {
            return status;
        }
        if (output_.size() != mixed.size()) {
            return make_error(ErrorCode::internal_error, "triple-balance size lost control-block alignment");
        }
        if (point_in_matrix_[i]) {
            continue;
        }
        const bool point = weights_[i] != 1.0F || targets_[i] != 1.0F;
        if (point) {
            point_output_.assign(mixed.size(), 0.0F);
            const render_common::AccumulateContext ctx{
                source.data(), &point_output_, start_frame, input_channels_, channels, default_interp_, 0};
            accumulate_point(i, ctx, frames, false);
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const float t = std::min(1.0F, static_cast<float>(frame + 1) / 512.0F);
            const float weight = weights_[i] + ((targets_[i] - weights_[i]) * t);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto sample = (frame * channels) + channel;
                float value = output_[sample];
                if (point) {
                    value = weight == 0 ? point_output_[sample]
                                        : (value * weight) + (point_output_[sample] * (1.0F - weight));
                }
                value *= track.output_gain;
                if (!live_gains.empty()) {
                    value *= live_gains[(frame * input_channels_) + track.input_channel];
                }
                mixed[sample] += value;
            }
        }
        weights_[i] = targets_[i];
    }
    return {};
}

} // namespace mradm::triple_balance
