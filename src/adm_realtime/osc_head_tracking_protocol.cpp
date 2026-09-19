#include "osc_head_tracking_protocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>

namespace mradm::realtime {
namespace {

bool read_string(std::span<const std::byte> data, std::size_t& offset, std::string_view expected) noexcept {
    const auto length = (expected.size() + 4U) & ~std::size_t{3};
    if (offset > data.size() || length > data.size() - offset) {
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        const auto value = i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0U;
        if (std::to_integer<unsigned char>(data[offset + i]) != value) {
            return false;
        }
    }
    offset += length;
    return true;
}

double read_float(std::span<const std::byte> data, std::size_t offset) noexcept {
    static_assert(sizeof(float) == 4U && std::numeric_limits<float>::is_iec559);
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4U; ++i) {
        bits = (bits << 8U) | std::to_integer<std::uint32_t>(data[offset + i]);
    }
    return static_cast<double>(std::bit_cast<float>(bits));
}

} // namespace

std::optional<HeadTrackingOrientation> decode_head_tracking_osc(std::span<const std::byte> data) noexcept {
    std::size_t offset = 0;
    const bool quaternion = read_string(data, offset, "/posebridge/v1/quaternion");
    if (!quaternion) {
        offset = 0;
        if (!read_string(data, offset, "/posebridge/v1/euler")) {
            return std::nullopt;
        }
    }
    const std::size_t count = quaternion ? 4U : 3U;
    if (!read_string(data, offset, quaternion ? ",ffff" : ",fff") || data.size() - offset != count * 4U) {
        return std::nullopt;
    }
    std::array<double, 4> q{};
    for (std::size_t i = 0; i < count; ++i) {
        q.at(i) = read_float(data, offset + (i * 4U));
        if (!std::isfinite(q.at(i))) {
            return std::nullopt;
        }
    }
    if (!quaternion) {
        // Fold before trig so finite but very large float32 angles do not lose all
        // useful range reduction precision or overflow an intermediate conversion.
        constexpr double k_half_radians = std::numbers::pi_v<double> / 360.0;
        const double yaw = std::remainder(q[0], 360.0) * k_half_radians;
        const double pitch = std::remainder(q[1], 360.0) * k_half_radians;
        const double roll = std::remainder(q[2], 360.0) * k_half_radians;
        const double cy = std::cos(yaw);
        const double sy = std::sin(yaw);
        const double cp = std::cos(pitch);
        const double sp = std::sin(pitch);
        const double cr = std::cos(roll);
        const double sr = std::sin(roll);
        q = {(cy * sp * cr) + (sy * cp * sr),
             (sy * cp * cr) - (cy * sp * sr),
             (cy * cp * sr) - (sy * sp * cr),
             (cy * cp * cr) + (sy * sp * sr)};
    }
    const double norm_squared = (q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]);
    if (norm_squared < 1.0e-12) {
        return std::nullopt;
    }
    const double norm = std::sqrt(norm_squared);
    std::ranges::transform(q, q.begin(), [norm](double component) { return component / norm; });
    const auto [x, y, z, w] = q;
    constexpr double k_degrees = 180.0 / std::numbers::pi_v<double>;
    HeadTrackingOrientation pose;
    std::ranges::transform(
        q, pose.quaternion_xyzw.begin(), [](double component) { return static_cast<float>(component); });
    pose.euler_deg = {
        static_cast<float>(std::atan2(2.0 * ((x * z) + (w * y)), 1.0 - (2.0 * ((x * x) + (y * y)))) * k_degrees),
        static_cast<float>(std::asin(std::clamp(2.0 * ((w * x) - (y * z)), -1.0, 1.0)) * k_degrees),
        static_cast<float>(std::atan2(2.0 * ((x * y) + (w * z)), 1.0 - (2.0 * ((x * x) + (z * z)))) * k_degrees)};
    return pose;
}

} // namespace mradm::realtime
