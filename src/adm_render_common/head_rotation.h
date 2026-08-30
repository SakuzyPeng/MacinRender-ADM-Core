#pragma once

#include <cmath>
#include <numbers>
#include <utility>

#include "adm/options.h"

namespace mradm::render_common {

// Precomputed world-to-head rotation shared by the batch/monitor and producer-neutral
// live binaural paths. ADM azimuth is positive left, elevation positive up.
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

} // namespace mradm::render_common
