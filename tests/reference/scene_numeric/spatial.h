#pragma once
// Frozen from 726db31; test-only, independent numerical reference.
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>
#include "adm/scene.h"
#include "adm/options.h"
namespace scene_numeric_reference {
using namespace mradm;
// Apply a ScenePositionOffset to a block position.  Returns the modified copy.
[[nodiscard]] inline SceneBlockPosition legacy_apply_position_offset(SceneBlockPosition pos, const ScenePositionOffset& off) {
    if (!pos.cartesian && !off.cartesian) {
        pos.azimuth += off.azimuth;
        pos.elevation = std::clamp(pos.elevation + off.elevation, -90.0F, 90.0F);
        pos.distance = std::max(0.0F, pos.distance + off.distance);
    } else if (pos.cartesian && off.cartesian) {
        pos.x += off.x;
        pos.y += off.y;
        pos.z += off.z;
    }
    return pos;
}

[[nodiscard]] inline float legacy_wrap_azimuth(float azimuth) {
    while (azimuth > 180.0F) {
        azimuth -= 360.0F;
    }
    while (azimuth <= -180.0F) {
        azimuth += 360.0F;
    }
    return azimuth;
}

[[nodiscard]] inline SceneBlockPosition legacy_scene_position_to_polar(const SceneBlockPosition& pos) {
    if (!pos.cartesian) {
        return pos;
    }

    const auto x = static_cast<double>(pos.x);
    const auto y = static_cast<double>(pos.y);
    const auto z = static_cast<double>(pos.z);
    const double xy = std::hypot(x, y);

    SceneBlockPosition polar;
    polar.cartesian = false;
    polar.azimuth = static_cast<float>(std::atan2(-x, y) * (180.0 / std::numbers::pi_v<double>) );
    polar.elevation = static_cast<float>(std::atan2(z, xy) * (180.0 / std::numbers::pi_v<double>) );
    polar.distance = static_cast<float>(std::sqrt((x * x) + (y * y) + (z * z)));
    return polar;
}

[[nodiscard]] inline SceneDirectionVector legacy_direction_vector_from_polar(float azimuth, float elevation) {
    const double az = static_cast<double>(azimuth) * (std::numbers::pi_v<double> / 180.0);
    const double el = static_cast<double>(elevation) * (std::numbers::pi_v<double> / 180.0);
    const double cos_el = std::cos(el);
    return {
        static_cast<float>(-std::sin(az) * cos_el),
        static_cast<float>(std::cos(az) * cos_el),
        static_cast<float>(std::sin(el)),
    };
}

[[nodiscard]] inline SceneDirectionVector legacy_direction_vector_from_position(const SceneBlockPosition& pos) {
    const auto polar = legacy_scene_position_to_polar(pos);
    return legacy_direction_vector_from_polar(polar.azimuth, polar.elevation);
}

[[nodiscard]] inline float legacy_direction_distance(const SceneDirectionVector& lhs, const SceneDirectionVector& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    const float dz = lhs.z - rhs.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

[[nodiscard]] inline std::optional<std::size_t>
legacy_nearest_non_lfe_speaker_index(const SceneBlockPosition& pos, const std::vector<SceneOutputSpeaker>& speakers) {
    std::optional<std::size_t> best_index;
    float best_distance = std::numeric_limits<float>::max();
    const auto src = legacy_direction_vector_from_position(pos);

    for (std::size_t i = 0; i < speakers.size(); ++i) {
        if (speakers[i].is_lfe) {
            continue;
        }
        const auto spk = legacy_direction_vector_from_polar(speakers[i].azimuth, speakers[i].elevation);
        const float dist = legacy_direction_distance(src, spk);
        if (dist < best_distance) {
            best_distance = dist;
            best_index = i;
        }
    }
    return best_index;
}

[[nodiscard]] inline SceneObjectBlock legacy_apply_channel_lock(const SceneObjectBlock& block,
                                                         const std::vector<SceneOutputSpeaker>& speakers) {
    if (!block.channel_lock) {
        return block;
    }
    const auto best_index = legacy_nearest_non_lfe_speaker_index(block.position, speakers);
    if (!best_index.has_value()) {
        return block;
    }

    const auto src = legacy_direction_vector_from_position(block.position);
    const auto& speaker = speakers[*best_index];
    const auto spk = legacy_direction_vector_from_polar(speaker.azimuth, speaker.elevation);
    if (block.channel_lock_max_distance &&
        legacy_direction_distance(src, spk) > (*block.channel_lock_max_distance + 1.0e-4F)) {
        return block;
    }

    SceneObjectBlock locked = block;
    const auto polar = legacy_scene_position_to_polar(block.position);
    locked.position.cartesian = false;
    locked.position.azimuth = speaker.azimuth;
    locked.position.elevation = speaker.elevation;
    locked.position.distance = polar.distance;
    return locked;
}

[[nodiscard]] inline std::vector<SceneObjectBlock> legacy_expand_object_divergence(const SceneObjectBlock& block) {
    const float divergence = std::clamp(block.divergence, 0.0F, 1.0F);
    if (divergence <= 1.0e-4F) {
        return {block};
    }

    SceneObjectBlock base = block;
    base.position = legacy_scene_position_to_polar(block.position);

    float divergence_angle = base.divergence_azimuth_range;
    if (block.position.cartesian && base.divergence_position_range > 0.0F) {
        divergence_angle =
            static_cast<float>(std::atan2(static_cast<double>(base.divergence_position_range),
                                          std::max(1.0e-6, static_cast<double>(base.position.distance))) *
                               (180.0 / std::numbers::pi_v<double>) );
    }
    divergence_angle = std::clamp(divergence_angle, 0.0F, 120.0F);

    const float side_weight = divergence / (divergence + 1.0F);
    const float center_weight = (1.0F - divergence) / (divergence + 1.0F);

    auto make_source = [&](float offset, float weight) {
        SceneObjectBlock out = base;
        out.position.azimuth = legacy_wrap_azimuth(base.position.azimuth + offset);
        out.gain *= weight;
        return out;
    };

    return {
        make_source(-divergence_angle, side_weight),
        make_source(0.0F, center_weight),
        make_source(divergence_angle, side_weight),
    };
}
[[nodiscard]] inline float canonical_vector_length(float x, float y, float z) noexcept {
    const double dx = x;
    const double dy = y;
    const double dz = z;
    return static_cast<float>(std::sqrt(((dx * dx) + (dy * dy)) + (dz * dz)));
}

struct ExtentDiskSample {
    float x{0.0F};
    float y{0.0F};
    float weight{0.0F}; // linear gain weight (active weights sum to 1)
};

inline constexpr float k_extent_disk_outer_weight = 1.0F / 12.0F; // outer ring total = 2/3
inline constexpr float k_extent_disk_inner_weight = 1.0F / 24.0F; // inner ring total = 1/3

inline constexpr std::array<ExtentDiskSample, 17> k_extent_disk_samples{{
    {0.0F, 0.0F, 0.0F},
    {1.0F, 0.0F, k_extent_disk_outer_weight},
    {-1.0F, 0.0F, k_extent_disk_outer_weight},
    {0.0F, 1.0F, k_extent_disk_outer_weight},
    {0.0F, -1.0F, k_extent_disk_outer_weight},
    {0.70710678F, 0.70710678F, k_extent_disk_outer_weight},
    {-0.70710678F, 0.70710678F, k_extent_disk_outer_weight},
    {0.70710678F, -0.70710678F, k_extent_disk_outer_weight},
    {-0.70710678F, -0.70710678F, k_extent_disk_outer_weight},
    {0.5F, 0.0F, k_extent_disk_inner_weight},
    {-0.5F, 0.0F, k_extent_disk_inner_weight},
    {0.0F, 0.5F, k_extent_disk_inner_weight},
    {0.0F, -0.5F, k_extent_disk_inner_weight},
    {0.35355339F, 0.35355339F, k_extent_disk_inner_weight},
    {-0.35355339F, 0.35355339F, k_extent_disk_inner_weight},
    {0.35355339F, -0.35355339F, k_extent_disk_inner_weight},
    {-0.35355339F, -0.35355339F, k_extent_disk_inner_weight},
}};


struct ExtentRadii { float width_radius; float height_radius; };
struct ExtentDirection { float azimuth; float elevation; float weight; };
[[nodiscard]] SceneDirectionVector vec_cross(const SceneDirectionVector& a, const SceneDirectionVector& b) noexcept {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] SceneDirectionVector vec_normalize(const SceneDirectionVector& v) noexcept {
    const float len = std::max(1.0e-6F, canonical_vector_length(v.x, v.y, v.z));
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
    const auto polar = legacy_scene_position_to_polar(position);
    const auto [width_radius, height_radius] = extent_disk_radii(width, height, depth, polar.distance);

    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        return {{polar.azimuth, polar.elevation, 1.0F}};
    }

    constexpr float k_deg2rad = std::numbers::pi_v<float> / 180.0F;

    const SceneDirectionVector center = legacy_direction_vector_from_position(position);
    SceneDirectionVector horizontal = vec_cross({0.0F, 0.0F, 1.0F}, center);
    if (canonical_vector_length(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
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

class HeadRotation {
  public:
    explicit HeadRotation(const ListenerOrientation& orientation) noexcept {
        constexpr double k_degrees_to_radians = std::numbers::pi_v<double> / 180.0;
        const Quaternion roll =
            axis_angle(0.0, 1.0, 0.0, static_cast<double>(orientation.roll_deg) * k_degrees_to_radians);
        const Quaternion pitch =
            axis_angle(1.0, 0.0, 0.0, static_cast<double>(orientation.pitch_deg) * k_degrees_to_radians);
        const Quaternion yaw =
            axis_angle(0.0, 0.0, 1.0, static_cast<double>(orientation.yaw_deg) * k_degrees_to_radians);
        const Quaternion head_to_world = multiply(yaw, multiply(pitch, roll));
        world_to_head_ = conjugate(head_to_world);
    }

    [[nodiscard]] std::pair<float, float> rotate_az_el(float azimuth_deg, float elevation_deg) const noexcept {
        constexpr double k_degrees_to_radians = std::numbers::pi_v<double> / 180.0;
        constexpr double k_radians_to_degrees = 180.0 / std::numbers::pi_v<double>;
        const double azimuth = static_cast<double>(azimuth_deg) * k_degrees_to_radians;
        const double elevation = static_cast<double>(elevation_deg) * k_degrees_to_radians;
        const double cos_elevation = std::cos(elevation);
        const Vector world{-std::sin(azimuth) * cos_elevation, std::cos(azimuth) * cos_elevation, std::sin(elevation)};
        const Vector head = apply(world);
        const double horizontal = std::hypot(head.x, head.y);
        return {static_cast<float>(std::atan2(-head.x, head.y) * k_radians_to_degrees),
                static_cast<float>(std::atan2(head.z, horizontal) * k_radians_to_degrees)};
    }

  private:
    struct Quaternion {
        double w{1.0};
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    struct Vector {
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    [[nodiscard]] static Quaternion multiply(const Quaternion& lhs, const Quaternion& rhs) noexcept {
        return {(lhs.w * rhs.w) - (lhs.x * rhs.x) - (lhs.y * rhs.y) - (lhs.z * rhs.z),
                (lhs.w * rhs.x) + (lhs.x * rhs.w) + (lhs.y * rhs.z) - (lhs.z * rhs.y),
                (lhs.w * rhs.y) - (lhs.x * rhs.z) + (lhs.y * rhs.w) + (lhs.z * rhs.x),
                (lhs.w * rhs.z) + (lhs.x * rhs.y) - (lhs.y * rhs.x) + (lhs.z * rhs.w)};
    }

    [[nodiscard]] static Quaternion conjugate(const Quaternion& value) noexcept {
        return {value.w, -value.x, -value.y, -value.z};
    }

    [[nodiscard]] static Quaternion axis_angle(double axis_x, double axis_y, double axis_z, double radians) noexcept {
        const double half = radians * 0.5;
        const double sine = std::sin(half);
        return {std::cos(half), axis_x * sine, axis_y * sine, axis_z * sine};
    }

    [[nodiscard]] Vector apply(const Vector& value) const noexcept {
        const Quaternion vector{0.0, value.x, value.y, value.z};
        const Quaternion rotated = multiply(multiply(world_to_head_, vector), conjugate(world_to_head_));
        return {rotated.x, rotated.y, rotated.z};
    }

    Quaternion world_to_head_;
};


struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] Vec3 add(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Vec3 scale(Vec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept {
    return {
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

[[nodiscard]] Vec3 normalize(Vec3 v) noexcept {
    const float n = canonical_vector_length(v.x, v.y, v.z);
    if (n <= 1.0e-8F) {
        return {0.0F, 1.0F, 0.0F};
    }
    return {v.x / n, v.y / n, v.z / n};
}

[[nodiscard]] Vec3 direction_from_position(const SceneBlockPosition& pos) noexcept {
    const auto polar = legacy_scene_position_to_polar(pos);
    const double az = static_cast<double>(polar.azimuth) * (std::numbers::pi_v<double> / 180.0);
    const double el = static_cast<double>(polar.elevation) * (std::numbers::pi_v<double> / 180.0);
    const double cos_el = std::cos(el);
    return normalize({
        static_cast<float>(-std::sin(az) * cos_el),
        static_cast<float>(std::cos(az) * cos_el),
        static_cast<float>(std::sin(el)),
    });
}

[[nodiscard]] float distance_from_position(const SceneBlockPosition& pos) noexcept {
    return pos.cartesian ? canonical_vector_length(pos.x, pos.y, pos.z) : pos.distance;
}

[[nodiscard]] std::pair<float, float> polar_from_direction(Vec3 dir) noexcept {
    const Vec3 n = normalize(dir);
    const double az =
        std::atan2(static_cast<double>(-n.x), static_cast<double>(n.y)) * (180.0 / std::numbers::pi_v<double>);
    const double el =
        std::atan2(static_cast<double>(n.z), std::hypot(static_cast<double>(n.x), static_cast<double>(n.y))) *
        (180.0 / std::numbers::pi_v<double>);
    return {static_cast<float>(az), static_cast<float>(el)};
}


struct ExtentSource { float azimuth; float elevation; float gain; size_t slot; };
constexpr size_t k_binaural_extent_center_slot=0;constexpr size_t k_binaural_extent_slots=17;
std::pair<float,float> block_position(const SceneObjectBlock& b){const auto p=legacy_scene_position_to_polar(b.position);return {p.azimuth,p.elevation};}
[[nodiscard]] std::vector<ExtentSource>
expand_binaural_extent(const SceneObjectBlock& block, float source_gain, BinauralSpreadMode spread_mode) {
    if (spread_mode == BinauralSpreadMode::none || spread_mode == BinauralSpreadMode::saf_spreader) {
        auto [az, el] = block_position(block);
        return {{az, el, source_gain, k_binaural_extent_center_slot}};
    }
    const float distance = distance_from_position(block.position);
    const auto [width_radius, height_radius] =
        extent_disk_radii(block.width, block.height, block.depth, distance);
    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        auto [az, el] = block_position(block);
        return {{az, el, source_gain, k_binaural_extent_center_slot}};
    }

    // Shared 17-point disk cloud (render_common); the per-backend geometry below
    // (direction_from_position / normalize / polar_from_direction in double precision)
    // stays local and unchanged so the binaural output remains bit-identical.
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const auto& k_samples = k_extent_disk_samples;

    const Vec3 center = direction_from_position(block.position);
    Vec3 horizontal = cross({0.0F, 0.0F, 1.0F}, center);
    if (canonical_vector_length(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
        horizontal = {1.0F, 0.0F, 0.0F};
    } else {
        horizontal = normalize(horizontal);
    }
    const Vec3 vertical = normalize(cross(center, horizontal));

    std::vector<ExtentSource> sources;
    sources.reserve(k_binaural_extent_slots - 1U);
    std::size_t slot = 0;
    for (const auto& sample : k_samples) {
        if (sample.weight <= 0.0F) {
            ++slot;
            continue;
        }
        const float h = std::tan(sample.x * width_radius * k_deg2rad);
        const float v = std::tan(sample.y * height_radius * k_deg2rad);
        auto [az, el] = polar_from_direction(normalize(add(add(center, scale(horizontal, h)), scale(vertical, v))));

        sources.push_back({az, el, source_gain * sample.weight, slot});
        ++slot;
    }
    return sources;
}

struct HeadVec {
    double x{};
    double y{};
    double z{};
};

struct HeadQuat {
    double w{1.0};
    double x{};
    double y{};
    double z{};
};

[[nodiscard]] HeadQuat quat_mul(const HeadQuat& a, const HeadQuat& b) {
    return {
        (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z),
        (a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
        (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
        (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
    };
}

[[nodiscard]] HeadQuat axis_angle(const HeadVec& axis, double radians) {
    const double half = radians * 0.5;
    const double s = std::sin(half);
    return {std::cos(half), axis.x * s, axis.y * s, axis.z * s};
}

[[nodiscard]] HeadVec rotate_by_quat(HeadVec v, const HeadQuat& q) {
    const HeadQuat p{0.0, v.x, v.y, v.z};
    const HeadQuat qc{q.w, -q.x, -q.y, -q.z};
    const HeadQuat r = quat_mul(quat_mul(q, p), qc);
    return {r.x, r.y, r.z};
}

// Head-lock 补偿:把一个总线的方向(SpatialMixer 约定:az +右、el +上)按听者头朝向预旋转,
// 使全局 HeadYaw/Pitch/Roll 对其恰好抵消 → 该源锁在头上(head-locked),不随转头移动。
// 坐标:x=右、y=前、z=上。组合顺序与 GUI 头部姿态反馈保持一致:roll(绕前轴 y) →
// pitch(绕右轴 x) → yaw(绕上轴 z)。yaw 已由 smoke/真机方向锁定;pitch/roll 仍建议真机标定。
[[nodiscard]] std::pair<float, float> head_lock_compensate(float az_deg, float el_deg, const ListenerOrientation& o) {
    constexpr double d2r = 0.017453292519943295;
    constexpr double r2d = 57.29577951308232;
    const double a = az_deg * d2r;
    const double e = el_deg * d2r;
    const HeadVec v{std::sin(a) * std::cos(e), std::cos(a) * std::cos(e), std::sin(e)};

    const HeadQuat q_roll = axis_angle({0.0, 1.0, 0.0}, o.roll_deg * d2r);
    const HeadQuat q_pitch = axis_angle({1.0, 0.0, 0.0}, o.pitch_deg * d2r);
    const HeadQuat q_yaw = axis_angle({0.0, 0.0, 1.0}, o.yaw_deg * d2r);
    const HeadQuat head_to_world = quat_mul(q_yaw, quat_mul(q_pitch, q_roll));
    const HeadVec rotated = rotate_by_quat(v, head_to_world);

    const auto az = static_cast<float>(std::atan2(rotated.x, rotated.y) * r2d);
    const auto el = static_cast<float>(std::asin(std::clamp(rotated.z, -1.0, 1.0)) * r2d);
    return {az, el};
}

// Set the four per-source SpatialMixer parameters for one input bus, short-circuiting on
// the first failure so a bad parameter set surfaces instead of silently continuing.

}
