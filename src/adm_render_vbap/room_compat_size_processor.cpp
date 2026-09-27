#include "room_compat_size_processor.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "room_compat_panner.h"

namespace mradm::room_compat {
// Ring, mode and channel indices are bounded by their fixed extents. Preserve
// the explicit scalar arithmetic order of the validated recursive structure.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index, readability-math-missing-parentheses)
namespace {
constexpr std::array<std::size_t, 4> k_delays{152, 200, 263, 346};
constexpr std::array<SizeDecorrelator::FilteredFrame, 4> k_coefficients{
    {{-0.4F, -0.4F, -0.4F, -0.4F}, {-0.4F, 0.4F, -0.4F, 0.4F}, {-0.4F, -0.4F, 0.4F, 0.4F}, {-0.4F, 0.4F, 0.4F, -0.4F}}};
constexpr std::array<int, 11> k_filter_indices{0, 0, -1, 1, 1, 2, 2, 3, 3, 0, 0};
constexpr std::array<float, 11> k_filter_signs{1, -1, 0, 1, -1, 1, -1, 1, -1, 1, -1};
constexpr std::array<std::size_t, 11> k_714_map{0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<std::size_t, 11> k_916_map{0, 1, 2, 4, 5, 6, 7, 10, 11, 14, 15};
} // namespace

SizeDecorrelator::SizeDecorrelator() {
    reset();
}

void SizeDecorrelator::reset_dsp() noexcept {
    for (auto& stage : delays_) {
        stage.fill({});
    }
    indices_.fill(0);
    input_delay_.fill(0);
    input_index_ = 0;
    pre_gain_ = 1;
    post_gain_ = 1;
    slow_level_ = 0;
    fast_level_ = 0;
    prefilter_state_ = 0;
    initial_ = true;
}

void SizeDecorrelator::reset() noexcept {
    reset_dsp();
    silent_blocks_ = 0;
}

void SizeDecorrelator::process(std::span<const float, subblock_frames> input,
                               std::span<FilteredFrame, subblock_frames> output) noexcept {
    const bool silent = std::ranges::all_of(input, [](float x) { return std::fabs(x) <= 0.00000011920928955078125F; });
    if (!silent) {
        silent_blocks_ = 0;
    } else if (silent_blocks_ > 15) {
        std::ranges::fill(output, FilteredFrame{});
        return;
    }
    for (std::size_t frame = 0; frame < subblock_frames; ++frame) {
        const float x = initial_ ? input[frame] * (static_cast<float>(frame) / 32.0F) : input[frame];
        const float pref = 0.6752336621284485F * (x + prefilter_state_);
        prefilter_state_ = pref - x;
        const float level = std::fabs(pref) + 1e-8F;
        fast_level_ = 0.00415802001953125F * level + 0.9958419799804688F * fast_level_;
        slow_level_ = 0.00026035308837890625F * level + 0.9997396469116211F * slow_level_;
        pre_gain_ = (pre_gain_ - 1) * 0.9995834231376648F + 1;
        post_gain_ = (post_gain_ - 1) * 0.9995834231376648F + 1;
        const float slow = slow_level_ == 0 ? 1e-8F : slow_level_;
        const float fast = fast_level_ == 0 ? 1e-8F : fast_level_;
        pre_gain_ = std::min(pre_gain_, slow * 1.1F / fast);
        post_gain_ = std::min(post_gain_, fast * 1.1F / slow);
        const float delayed = input_delay_[input_index_];
        input_delay_[input_index_] = x;
        input_index_ = (input_index_ + 1) % input_delay_.size();
        FilteredFrame signal;
        signal.fill(delayed * pre_gain_);
        for (std::size_t stage = 0; stage < k_delays.size(); ++stage) {
            auto& memory = delays_[stage][indices_[stage]];
            for (std::size_t mode = 0; mode < signal.size(); ++mode) {
                const float next = k_coefficients[stage][mode] * memory[mode] + signal[mode];
                signal[mode] = memory[mode] - k_coefficients[stage][mode] * next;
                memory[mode] = next;
            }
            indices_[stage] = (indices_[stage] + 1) % k_delays[stage];
        }
        const float fade = silent && silent_blocks_ == 15 ? static_cast<float>(32 - frame) / 32.0F : 1.0F;
        for (std::size_t mode = 0; mode < signal.size(); ++mode) {
            output[frame][mode] = (signal[mode] * post_gain_) * fade;
        }
    }
    initial_ = false;
    if (silent) {
        if (silent_blocks_ == 15) {
            reset_dsp();
        }
        ++silent_blocks_;
    }
}

Result<SizeObjectProcessor>
SizeObjectProcessor::create(std::span<const SizeEvent> events, std::string layout, uint32_t sample_rate) {
    if (sample_rate != 48000 || (layout != "7.1.4" && layout != "4+7+0" && layout != "9.1.6" && !is_room_222(layout))) {
        return make_error(ErrorCode::unsupported, "room size supports 48 kHz 7.1.4/9.1.6 and experimental 22.2");
    }
    if (events.empty() || events.front().start_sample != 0) {
        return make_error(ErrorCode::unsupported, "room-compat size requires an event at frame zero");
    }
    uint64_t previous = 0;
    bool initial = true;
    for (const auto& event : events) {
        if ((!initial && event.start_sample / 512 == previous / 512) || (!initial && event.start_sample <= previous)) {
            return make_error(ErrorCode::unsupported, "room-compat size allows one ordered event per 512-frame block");
        }
        if (is_room_222(layout)) {
            SceneBlockPosition position;
            position.cartesian = true;
            position.x = event.position.x * 2 - 1;
            position.y = 1 - event.position.y * 2;
            position.z = event.position.z;
            const auto valid = room_222_gains(position, event.size);
            if (!valid) {
                return tl::unexpected{valid.error()};
            }
        } else {
            const auto valid = quantize_size_parameters(event.position, event.size);
            if (!valid) {
                return tl::unexpected{valid.error()};
            }
        }
        initial = false;
        previous = event.start_sample;
    }
    SizeObjectProcessor result;
    result.events_.assign(events.begin(), events.end());
    result.layout_ = std::move(layout);
    result.channels_ = result.layout_ == "9.1.6" ? 16 : 12;
    if (is_room_222(result.layout_)) {
        result.channels_ = k_room_222_channels;
    }
    result.reset();
    return result;
}

void SizeObjectProcessor::reset() noexcept {
    next_event_ = 1;
    control_start_ = 0;
    pending_frames_ = 0;
    pending_.fill(0);
    finished_ = false;
    first_ = true;
    previous_mix_ = {};
    previous_extended_mix_ = {};
    cached_size_ = -1;
    previous_point_.fill(0);
    previous_filter_active_ = false;
    older_filter_active_ = false;
    decorrelator_.reset();
    if (!events_.empty()) {
        position_ = target_position_ = events_[0].position;
        size_ = target_size_ = events_[0].size;
    }
}

Result<void> SizeObjectProcessor::push(std::span<const float> input, std::vector<float>& output) {
    if (finished_) {
        return make_error(ErrorCode::invalid_argument, "size processor requires reset after finish");
    }
    if (!std::ranges::all_of(input, [](float x) { return std::isfinite(x); })) {
        return make_error(ErrorCode::invalid_argument, "size processor received nonfinite PCM");
    }
    while (!input.empty()) {
        const auto take = std::min(input.size(), pending_.size() - pending_frames_);
        std::copy_n(input.begin(), take, pending_.begin() + static_cast<std::ptrdiff_t>(pending_frames_));
        pending_frames_ += take;
        input = input.subspan(take);
        if (pending_frames_ == pending_.size()) {
            auto processed = process_control(output, pending_.size());
            if (!processed) {
                return processed;
            }
            pending_frames_ = 0;
        }
    }
    return {};
}

Result<void> SizeObjectProcessor::finish(std::vector<float>& output) {
    if (finished_) {
        return make_error(ErrorCode::invalid_argument, "size processor already finished");
    }
    if (pending_frames_ != 0) {
        std::fill(pending_.begin() + static_cast<std::ptrdiff_t>(pending_frames_), pending_.end(), 0);
        auto result = process_control(output, pending_frames_);
        if (!result) {
            return result;
        }
    }
    finished_ = true;
    pending_frames_ = 0;
    return {};
}

// NOLINTNEXTLINE(readability-function-size): retain verified native arithmetic and ordering without refactoring.
Result<void> SizeObjectProcessor::process_control(std::vector<float>& output, std::size_t valid_frames) {
    if (channels_ == k_room_222_channels) {
        return process_extended_control(output, valid_frames);
    }
    if (next_event_ < events_.size() && events_[next_event_].start_sample < control_start_ + 512) {
        const auto& event = events_[next_event_++];
        target_position_ = event.position;
        target_size_ = event.size;
    }
    if (!first_) {
        const float position_alpha = 1 - std::exp(-512.0F / 1200.0F);
        const float size_alpha = 1 - std::exp(-512.0F / 960.0F);
        position_.x += position_alpha * (target_position_.x - position_.x);
        position_.y += position_alpha * (target_position_.y - position_.y);
        position_.z += position_alpha * (target_position_.z - position_.z);
        size_ += size_alpha * (target_size_ - size_);
        if (target_size_ == 0 && size_ < 0.005F) {
            size_ = 0;
        }
    }
    auto quantized = quantize_size_parameters(position_, size_);
    if (!quantized) {
        return tl::unexpected{quantized.error()};
    }
    auto spatial = raw_size_gains(*quantized);
    if (!spatial) {
        return tl::unexpected{spatial.error()};
    }
    const auto mix = mix_size_gains(*spatial, size_);
    SceneBlockPosition point_position;
    point_position.cartesian = true;
    point_position.x = position_.x * 2 - 1;
    point_position.y = 1 - position_.y * 2;
    // OMO clears the direct event's elevation-enabled flag when its direct
    // coefficient reaches zero. OAR still advances that (silent) point state;
    // restoring a smaller size must interpolate back from the floor gains.
    point_position.z = mix.direct == 0 ? 0 : position_.z;
    auto point = point_gains(point_position, 1, layout_);
    if (!point) {
        return tl::unexpected{point.error()};
    }
    if (first_) {
        previous_mix_ = mix;
        std::copy(point->begin(), point->end(), previous_point_.begin());
    }
    std::array<SizeDecorrelator::FilteredFrame, 512> filtered{};
    const bool filter_active = size_ > 0;
    if (filter_active || previous_filter_active_ || older_filter_active_) {
        for (std::size_t frame = 0; frame < 512; frame += 32) {
            decorrelator_.process(std::span<const float, 32>(pending_.data() + frame, 32),
                                  std::span<SizeDecorrelator::FilteredFrame, 32>(filtered.data() + frame, 32));
        }
    } else {
        decorrelator_.reset();
    }
    older_filter_active_ = previous_filter_active_;
    previous_filter_active_ = filter_active;
    const auto& map = channels_ == 16 ? k_916_map : k_714_map;
    const auto begin = output.size();
    output.resize(begin + valid_frames * channels_, 0);
    for (std::size_t frame = 0; frame < valid_frames; ++frame) {
        const float now = static_cast<float>(frame + 1) / 512.0F;
        const float before = 1 - now;
        const float dry = pending_[frame] * (previous_mix_.direct * before + mix.direct * now);
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            output[begin + frame * channels_ + channel] =
                dry * (previous_point_[channel] * before + (*point)[channel] * now);
        }
        for (std::size_t channel = 0; channel < 11; ++channel) {
            const float gain = previous_mix_.spread[channel] * before + mix.spread[channel] * now;
            float sample = pending_[frame];
            if (k_filter_indices[channel] >= 0) {
                sample = 0.9219544529914856F * sample +
                         0.3872983455657959F * k_filter_signs[channel] *
                             filtered[frame][static_cast<std::size_t>(k_filter_indices[channel])];
            }
            output[begin + frame * channels_ + map[channel]] += gain * sample;
        }
    }
    // The zero-size control block still emits the previous spread through its
    // interpolation ramp. Reset only after mixing that block, so a subsequent
    // nonzero update starts cold instead of reusing an inaudible warm filter.
    if (!filter_active) {
        decorrelator_.reset();
    }
    first_ = false;
    previous_mix_ = mix;
    std::copy(point->begin(), point->end(), previous_point_.begin());
    control_start_ += 512;
    return {};
}

// The reference layouts above retain their exact arithmetic and fixed native
// branch mapping. This separate layout-driven extension shares filter state,
// control timing and smoothing with its own 22.2 geometry.
Result<void> SizeObjectProcessor::process_extended_control(std::vector<float>& output, std::size_t valid_frames) {
    if (next_event_ < events_.size() && events_[next_event_].start_sample < control_start_ + 512) {
        const auto& event = events_[next_event_++];
        target_position_ = event.position;
        target_size_ = event.size;
    }
    if (!first_) {
        const float position_alpha = 1 - std::exp(-512.0F / 1200.0F);
        const float size_alpha = 1 - std::exp(-512.0F / 960.0F);
        position_.x += position_alpha * (target_position_.x - position_.x);
        position_.y += position_alpha * (target_position_.y - position_.y);
        position_.z += position_alpha * (target_position_.z - position_.z);
        size_ += size_alpha * (target_size_ - size_);
        if (target_size_ == 0 && size_ < .005F) {
            size_ = 0;
        }
    }
    if (cached_size_ != size_ || cached_position_.x != position_.x || cached_position_.y != position_.y ||
        cached_position_.z != position_.z) {
        SceneBlockPosition position;
        position.cartesian = true;
        position.x = position_.x * 2 - 1;
        position.y = 1 - position_.y * 2;
        position.z = position_.z;
        auto spatial = room_222_gains(position, size_);
        if (!spatial) {
            return tl::unexpected{spatial.error()};
        }
        cached_extended_mix_ = room_222_mix(*spatial, size_);
        position.z = cached_extended_mix_.direct == 0 ? 0 : position.z;
        auto point = room_222_gains(position);
        if (!point) {
            return tl::unexpected{point.error()};
        }
        cached_extended_point_ = *point;
        cached_position_ = position_;
        cached_size_ = size_;
    }
    const auto& mix = cached_extended_mix_;
    if (first_) {
        previous_extended_mix_ = mix;
        previous_point_ = cached_extended_point_;
    }
    std::array<SizeDecorrelator::FilteredFrame, 512> filtered{};
    const bool filter_active = size_ > 0;
    if (filter_active || previous_filter_active_ || older_filter_active_) {
        for (std::size_t frame = 0; frame < 512; frame += 32) {
            decorrelator_.process(std::span<const float, 32>(pending_.data() + frame, 32),
                                  std::span<SizeDecorrelator::FilteredFrame, 32>(filtered.data() + frame, 32));
        }
    } else {
        decorrelator_.reset();
    }
    older_filter_active_ = previous_filter_active_;
    previous_filter_active_ = filter_active;
    const auto begin = output.size();
    output.resize(begin + valid_frames * channels_, 0);
    for (std::size_t frame = 0; frame < valid_frames; ++frame) {
        const float now = static_cast<float>(frame + 1) / 512;
        const float before = 1 - now;
        const float dry = pending_[frame] * (previous_extended_mix_.direct * before + mix.direct * now);
        for (const auto& node : room_222_nodes()) {
            const auto channel = node.channel;
            float wet = pending_[frame];
            if (node.filter >= 0) {
                wet = .9219544529914856F * wet +
                      .3872983455657959F * node.sign * filtered[frame][static_cast<std::size_t>(node.filter)];
            }
            const float gain = previous_extended_mix_.spread[channel] * before + mix.spread[channel] * now;
            output[begin + frame * channels_ + channel] =
                dry * (previous_point_[channel] * before + cached_extended_point_[channel] * now) + gain * wet;
        }
    }
    if (!filter_active) {
        decorrelator_.reset();
    }
    first_ = false;
    previous_extended_mix_ = mix;
    previous_point_ = cached_extended_point_;
    control_start_ += 512;
    return {};
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index, readability-math-missing-parentheses)
} // namespace mradm::room_compat
