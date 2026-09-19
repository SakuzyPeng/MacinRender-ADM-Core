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

std::uint64_t read_integer(std::span<const std::byte> data, std::size_t& offset, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(data[offset++]);
    }
    return value;
}

double read_float(std::span<const std::byte> data, std::size_t offset) noexcept {
    static_assert(sizeof(float) == 4U && std::numeric_limits<float>::is_iec559);
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4U; ++i) {
        bits = (bits << 8U) | std::to_integer<std::uint32_t>(data[offset + i]);
    }
    return static_cast<double>(std::bit_cast<float>(bits));
}

// Caller has checked the exact metadata and pose payload size.
bool decode_metadata(std::span<const std::byte> data, std::size_t& offset, HeadTrackingTiming& timing) noexcept {
    timing.source_session_id = read_integer(data, offset, 8U);
    timing.source_sequence = read_integer(data, offset, 8U);
    timing.source_received_ns = read_integer(data, offset, 8U);
    timing.sample_time_ms = read_integer(data, offset, 8U);
    timing.sample_time_kind = static_cast<std::uint32_t>(read_integer(data, offset, 4U));
    timing.sample_clock_epoch = read_integer(data, offset, 8U);
    constexpr auto k_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::array fields{timing.source_session_id,
                            timing.source_sequence,
                            timing.source_received_ns,
                            timing.sample_time_ms,
                            timing.sample_clock_epoch};
    // OSC h is signed; negative fields are not valid metadata.
    const bool nonnegative = std::ranges::all_of(fields, [](auto value) { return value <= k_max; });
    const bool absent = timing.sample_time_kind == 0U && timing.sample_time_ms == 0U && timing.sample_clock_epoch == 0U;
    const bool present =
        (timing.sample_time_kind == 1U || timing.sample_time_kind == 2U) && timing.sample_clock_epoch > 0U;
    return nonnegative && timing.source_session_id > 0U && timing.source_sequence > 0U && (absent || present);
}

} // namespace

std::optional<HeadTrackingMessage> decode_head_tracking_osc(std::span<const std::byte> data) noexcept {
    std::size_t offset = 0;
    bool quaternion = false;
    HeadTrackingTiming timing;
    for (const auto version : {1U, 2U}) {
        offset = 0;
        quaternion =
            read_string(data, offset, version == 1U ? "/posebridge/v1/quaternion" : "/posebridge/v2/quaternion");
        if (!quaternion) {
            offset = 0;
            if (!read_string(data, offset, version == 1U ? "/posebridge/v1/euler" : "/posebridge/v2/euler")) {
                continue;
            }
        }
        timing.protocol_version = version;
        break;
    }
    if (timing.protocol_version == 0U) {
        return std::nullopt;
    }
    const bool v2 = timing.protocol_version == 2U;
    const std::size_t count = quaternion ? 4U : 3U;
    const std::string_view quaternion_tags = v2 ? ",hhhhihffff" : ",ffff";
    const std::string_view euler_tags = v2 ? ",hhhhihfff" : ",fff";
    const auto tags = quaternion ? quaternion_tags : euler_tags;
    if (!read_string(data, offset, tags) || data.size() - offset != (v2 ? 44U : 0U) + (count * 4U)) {
        return std::nullopt;
    }
    if (v2 && !decode_metadata(data, offset, timing)) {
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
    return HeadTrackingMessage{pose, timing};
}

bool OscSourceOrder::accept(const HeadTrackingTiming& timing) noexcept {
    if (timing.protocol_version == 1U) {
        return true;
    }
    const bool new_source = timing.source_session_id != last_source_.source_session_id;
    if (new_source) {
        if (std::ranges::find(retired_, timing.source_session_id) != retired_.end()) {
            return false;
        }
    } else {
        if (timing.source_sequence <= last_source_.source_sequence ||
            timing.source_received_ns < last_source_.source_received_ns) {
            return false;
        }
        if (timing.sample_time_kind != 0U && last_clock_.sample_time_kind != 0U) {
            if (timing.sample_clock_epoch < last_clock_.sample_clock_epoch ||
                (timing.sample_clock_epoch == last_clock_.sample_clock_epoch &&
                 (timing.sample_time_kind != last_clock_.sample_time_kind ||
                  timing.sample_time_ms <= last_clock_.sample_time_ms))) {
                return false;
            }
        }
    }
    // Commit only after every check; a rejected packet cannot advance any anchor.
    if (new_source) {
        retired_.at(retired_next_) = last_source_.source_session_id;
        retired_next_ = (retired_next_ + 1U) % retired_.size();
        last_clock_ = {};
    }
    last_source_ = timing;
    if (timing.sample_time_kind != 0U) {
        last_clock_ = timing;
    }
    return true;
}

} // namespace mradm::realtime
