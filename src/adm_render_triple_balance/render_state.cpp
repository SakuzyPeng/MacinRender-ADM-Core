#include "render_state.h"

#include <algorithm>
#include <cmath>
#include <exception>
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
        }
    }
    if (live_points) {
        std::vector<std::size_t> channels;
        channels.reserve(prepared.size_tracks.size());
        for (const auto& track : prepared.size_tracks) {
            channels.push_back(track.input_channel);
        }
        result.point_mix_.emplace(dsp::PcmMixer::dynamic(
            result.input_channels_, prepared.output_channels, channels, 3U, 1024U, result.default_interp_));
        result.point_blocks_.reserve(3U);
        result.point_gains_.reserve(std::size_t{3U} * prepared.output_channels);
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
    if (point_mix_) {
        point_mix_->reset();
    }
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
    if (point_mix_) {
        point_mix_->reset();
    }
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
    if (points_.empty()) {
        return {};
    }
    if (!point_mix_) {
        return make_error(ErrorCode::internal_error, "Unprepared point mixer");
    }
    const float alpha = 1.0F - std::exp(-512.0F / 1200.0F);
    for (std::size_t i = 0; i < points_.size(); ++i) {
        auto& state = points_[i];
        const auto& events = prepared_->size_tracks[i].events;
        if (state.control_start != start_frame) {
            return make_error(ErrorCode::internal_error, "Triple Balance point state lost block alignment");
        }
        point_blocks_.clear();
        point_gains_.clear();
        point_blocks_.push_back({0, std::numeric_limits<uint64_t>::max(), 0, 3U});
        point_gains_.insert(point_gains_.end(), state.gains.begin(), state.gains.begin() + prepared_->output_channels);
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
                    point_blocks_.push_back({state.control_start, std::numeric_limits<uint64_t>::max(), 512, 6U});
                    point_gains_.insert(point_gains_.end(), gains->begin(), gains->end());
                }
            }
            state.control_start += 512;
        }
        point_mix_->update(i, point_blocks_, point_gains_, prepared_->size_tracks[i].output_gain);
    }
    return {};
}

void SizeMixer::accumulate_point(std::size_t track,
                                 const render_common::AccumulateContext& context,
                                 std::size_t frames,
                                 bool user_gain) {
    if (!point_mix_) {
        std::terminate();
    }
    point_mix_->speaker(std::span{context.input, frames * context.num_in_ch},
                        *context.output,
                        context.live_gains,
                        context.frames_done,
                        frames,
                        track,
                        user_gain ? std::nullopt : std::optional<float>{1.0F});
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
