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

bool is_lfe_label(std::string_view raw) noexcept {
    std::string key;
    key.reserve(raw.size());
    for (const char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
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
