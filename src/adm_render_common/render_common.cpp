#include "render_common.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace mradm::render_common {

namespace {

[[nodiscard]] SceneDirectionVector vec_cross(const SceneDirectionVector& a, const SceneDirectionVector& b) noexcept {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] SceneDirectionVector vec_normalize(const SceneDirectionVector& v) noexcept {
    const float len = std::max(1.0e-6F, std::hypot(v.x, v.y, v.z));
    return {v.x / len, v.y / len, v.z / len};
}

// Inverse of direction_vector_from_polar: recover (azimuth, elevation) in degrees,
// project convention (azimuth +ve = left).
[[nodiscard]] std::pair<float, float> polar_from_direction(const SceneDirectionVector& dir) noexcept {
    constexpr float k_rad2deg = 180.0F / std::numbers::pi_v<float>;
    const float azimuth = std::atan2(-dir.x, dir.y) * k_rad2deg;
    const float elevation = std::atan2(dir.z, std::hypot(dir.x, dir.y)) * k_rad2deg;
    return {azimuth, elevation};
}

} // namespace

std::string normalise_speaker_label_key(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

std::string canonicalise_speaker_label(std::string_view raw) {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-') {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return key;
}

std::optional<float> resolve_live_channel_gain(const LiveOverrides& overrides,
                                               std::string_view object_id,
                                               std::string_view channel_label_key) {
    const LiveObjectOverride* whole = nullptr;
    const LiveObjectOverride* specific = nullptr;
    for (const auto& ov : overrides.objects) {
        if (ov.object_id != object_id) {
            continue;
        }
        if (ov.speaker_label.empty()) {
            whole = &ov; // whole-object override (legacy / Objects / bed-wide)
        } else if (!channel_label_key.empty() && canonicalise_speaker_label(ov.speaker_label) == channel_label_key) {
            specific = &ov; // per-channel override wins over the whole-object one
        }
    }
    const LiveObjectOverride* pick = (specific != nullptr) ? specific : whole;
    if (pick == nullptr) {
        return std::nullopt;
    }
    if (pick->mute) {
        return 0.0F;
    }
    return std::pow(10.0F, pick->gain_db / 20.0F);
}

bool resolve_live_head_locked(const LiveOverrides& overrides,
                              std::string_view object_id,
                              std::string_view channel_label_key) {
    const LiveObjectOverride* whole = nullptr;
    const LiveObjectOverride* specific = nullptr;
    for (const auto& ov : overrides.objects) {
        if (ov.object_id != object_id) {
            continue;
        }
        if (ov.speaker_label.empty()) {
            whole = &ov;
        } else if (!channel_label_key.empty() && canonicalise_speaker_label(ov.speaker_label) == channel_label_key) {
            specific = &ov;
        }
    }
    const LiveObjectOverride* pick = (specific != nullptr) ? specific : whole;
    return pick != nullptr && pick->head_locked;
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

bool is_lfe_label(std::string_view raw) noexcept {
    const std::string key = normalise_speaker_label_key(raw);
    return key == "LF" || key.find("LFE") != std::string::npos || key.find("SUB") != std::string::npos ||
           key.find("LOWFREQUENCY") != std::string::npos;
}

bool any_label_is_lfe(const std::vector<std::string>& labels) noexcept {
    return std::ranges::any_of(labels, [](const auto& label) { return is_lfe_label(label); });
}

bool direct_speakers_block_is_lfe(const SceneDirectSpeakersBlock& block) noexcept {
    return block.low_pass_hz.has_value() || any_label_is_lfe(block.speaker_labels);
}

SerialWorker::SerialWorker() {
    thread_ = std::thread([this] { run(); });
}

SerialWorker::~SerialWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::future<void> SerialWorker::post(std::function<void()> task) {
    std::packaged_task<void()> packaged(std::move(task));
    std::future<void> future = packaged.get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(packaged));
    }
    cv_.notify_one();
    return future;
}

void SerialWorker::run() {
    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return; // stop_ is set and nothing left to drain
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                         const SceneObject& object,
                                         const std::vector<SceneOutputSpeaker>& speakers,
                                         LogSink& logs,
                                         std::string_view log_module,
                                         bool& screen_ref_warned) {
    SceneObjectBlock block = raw_block;
    if (object.position_offset) {
        block.position = apply_position_offset(block.position, *object.position_offset);
    }
    if (block.screen_ref && !screen_ref_warned) {
        logs.log(LogLevel::warning,
                 log_module,
                 "screenRef requires referenceScreen geometry; rendering block as screenRef=false");
        screen_ref_warned = true;
    }
    block = apply_channel_lock(block, speakers);

    return {
        expand_object_divergence(block),
        raw_block.start_sample,
        std::min(raw_block.end_sample, object.end_sample),
        raw_block.jump_position,
        raw_block.interp_length_samples,
    };
}

ExtentRadii extent_disk_radii(float width, float height, float depth, float distance) {
    // Distance-dependent spread scaling: nearer objects subtend a wider angle.
    const float spread_scale = std::clamp(1.0F / std::max(0.4F, distance), 0.5F, 2.5F);
    const float depth_radius = std::max(0.0F, depth) * 20.0F * spread_scale;
    const float width_radius = (std::max(0.0F, width) * 60.0F * spread_scale) + depth_radius;
    const float height_radius = (std::max(0.0F, height) * 45.0F * spread_scale) + depth_radius;
    return {width_radius, height_radius};
}

std::vector<ExtentDirection>
extent_disk_cloud(const SceneBlockPosition& position, float width, float height, float depth) {
    const auto polar = scene_position_to_polar(position);
    const auto [width_radius, height_radius] = extent_disk_radii(width, height, depth, polar.distance);

    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        return {{polar.azimuth, polar.elevation, 1.0F}};
    }

    constexpr float k_deg2rad = std::numbers::pi_v<float> / 180.0F;

    const SceneDirectionVector center = direction_vector_from_position(position);
    SceneDirectionVector horizontal = vec_cross({0.0F, 0.0F, 1.0F}, center);
    if (std::hypot(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
        horizontal = {1.0F, 0.0F, 0.0F};
    } else {
        horizontal = vec_normalize(horizontal);
    }
    const SceneDirectionVector vertical = vec_normalize(vec_cross(center, horizontal));

    std::vector<ExtentDirection> cloud;
    cloud.reserve(k_extent_disk_samples.size() - 1U);
    for (const auto& sample : k_extent_disk_samples) {
        if (sample.weight <= 0.0F) {
            continue;
        }
        const float h = std::tan(sample.x * width_radius * k_deg2rad);
        const float v = std::tan(sample.y * height_radius * k_deg2rad);
        const SceneDirectionVector dir = vec_normalize({(center.x + (horizontal.x * h)) + (vertical.x * v),
                                                        (center.y + (horizontal.y * h)) + (vertical.y * v),
                                                        (center.z + (horizontal.z * h)) + (vertical.z * v)});
        const auto [azimuth, elevation] = polar_from_direction(dir);
        cloud.push_back({azimuth, elevation, sample.weight});
    }
    return cloud;
}

} // namespace mradm::render_common
