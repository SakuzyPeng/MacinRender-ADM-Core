#include "osc_head_tracking_protocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>

#include <nlohmann/json.hpp>

namespace mradm::realtime {
namespace {

std::optional<std::string_view> read_string(std::span<const std::byte> data, std::size_t& offset) noexcept {
    const auto begin = offset;
    while (offset < data.size() && data[offset] != std::byte{0}) {
        ++offset;
    }
    if (offset == data.size()) {
        return std::nullopt;
    }
    const auto end = offset;
    const auto aligned = (offset + 4U) & ~std::size_t{3};
    if (aligned > data.size()) {
        return std::nullopt;
    }
    while (offset < aligned) {
        if (data[offset++] != std::byte{0}) {
            return std::nullopt;
        }
    }
    return std::string_view{reinterpret_cast<const char*>(data.data() + begin), end - begin};
}
std::uint64_t read_integer(std::span<const std::byte> data, std::size_t& offset, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(data[offset++]);
    }
    return value;
}
double read_float(std::span<const std::byte> data, std::size_t offset) noexcept {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4U; ++i) {
        bits = (bits << 8U) | std::to_integer<std::uint32_t>(data[offset + i]);
    }
    return static_cast<double>(std::bit_cast<float>(bits));
}
bool signed_range(std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

std::optional<HeadTrackingOrientation>
orientation(std::span<const std::byte> data, std::size_t offset, bool quaternion) noexcept {
    const std::size_t count = quaternion ? 4U : 3U;
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
    const double sin_pitch = std::clamp(2.0 * ((w * x) - (y * z)), -1.0, 1.0);
    double yaw = 0.0;
    double roll = 0.0;
    if (std::abs(sin_pitch) >= 1.0 - 1e-12) {
        // At either pole yaw and roll are coupled. Choose zero roll and retain
        // their combined heading instead of evaluating two unstable atan2(0, 0).
        yaw = std::atan2(2.0 * ((w * y) - (x * z)), 1.0 - (2.0 * ((y * y) + (z * z))));
    } else {
        yaw = std::atan2(2.0 * ((x * z) + (w * y)), 1.0 - (2.0 * ((x * x) + (y * y))));
        roll = std::atan2(2.0 * ((x * y) + (w * z)), 1.0 - (2.0 * ((x * x) + (z * z))));
    }
    pose.euler_deg = {static_cast<float>(yaw * k_degrees),
                      static_cast<float>(std::asin(sin_pitch) * k_degrees),
                      static_cast<float>(roll * k_degrees)};
    return pose;
}

std::optional<std::uint64_t> decimal(const nlohmann::json& object, const char* key, bool positive) {
    const auto field = object.find(key);
    if (field == object.end() || !field->is_string()) {
        return std::nullopt;
    }
    const auto& value = field->get_ref<const std::string&>();
    if (value.empty() || (value.size() > 1U && value.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !signed_range(number) ||
        (positive && number == 0U)) {
        return std::nullopt;
    }
    return number;
}

std::optional<HeadTrackingMessage> telemetry(std::string_view address, std::string_view text) {
    bool too_deep = false;
    const auto value = nlohmann::json::parse(
        text,
        [&too_deep](int depth, auto, auto&) {
            if (depth > 16) {
                too_deep = true;
                return false;
            }
            return true;
        },
        false);
    if (too_deep || value.is_discarded() || !value.is_object() || !value.contains("schema") ||
        !value["schema"].is_number_unsigned()) {
        return std::nullopt;
    }
    HeadTrackingMessage msg;
    if (value["schema"] != k_posebridge_protocol) {
        msg.kind = HeadTrackingMessageKind::incompatible;
        return msg;
    }
    const bool info = address == "/posebridge/info";
    if (value.value("kind", std::string{}) != (info ? "info" : "status") || !value.contains("source_id") ||
        !value["source_id"].is_string()) {
        return std::nullopt;
    }
    msg.source_id = value["source_id"].get<std::string>();
    const auto instance = decimal(value, "instance_id", true);
    const auto session = decimal(value, "session_id", false);
    const auto revision = decimal(value, "metadata_revision", true);
    const auto reference = decimal(value, "reference_epoch", true);
    const auto sequence = decimal(value, "message_seq", true);
    if (!valid_source_id(msg.source_id) || !instance || !session || !revision || !reference || !sequence) {
        return std::nullopt;
    }
    msg.kind = info ? HeadTrackingMessageKind::info : HeadTrackingMessageKind::status;
    msg.timing.protocol_version = k_posebridge_protocol;
    msg.timing.instance_id = *instance;
    msg.timing.source_session_id = *session;
    msg.timing.metadata_revision = *revision;
    msg.timing.reference_epoch = *reference;
    msg.message_sequence = *sequence;
    const char* payload_key = info ? "descriptor" : "status";
    if (!value.contains(payload_key) || !value[payload_key].is_object()) {
        return std::nullopt;
    }
    const auto& payload = value[payload_key];
    if (decimal(payload, "session_id", false) != session) {
        return std::nullopt;
    }
    if (info) {
        if (payload.value("source_id", std::string{}) != msg.source_id ||
            decimal(payload, "instance_id", true) != instance ||
            decimal(payload, "metadata_revision", true) != revision ||
            decimal(payload, "reference_epoch", true) != reference ||
            payload.value("coordinate_profile", std::string{}) != "posebridge.yxz.v1") {
            return std::nullopt;
        }
    } else {
        const auto state = payload.value("state", std::string{});
        constexpr std::array states{"idle",
                                    "scanning",
                                    "connecting",
                                    "active",
                                    "stale",
                                    "reconnecting",
                                    "stopped",
                                    "failed",
                                    "configuring",
                                    "complete",
                                    "inspecting"};
        const auto samples = decimal(payload, "session_samples", false);
        if (!samples || std::ranges::find(states, state) == states.end()) {
            return std::nullopt;
        }
        msg.source_active = state == "active";
        msg.reported_samples = *samples;
    }
    msg.json = std::string{text};
    return msg;
}

std::optional<HeadTrackingMessage> pose_message(std::span<const std::byte> data, std::size_t offset, bool quaternion) {
    if (data.size() - offset < 4U) {
        return std::nullopt;
    }
    HeadTrackingMessage msg;
    if (read_integer(data, offset, 4U) != k_posebridge_protocol) {
        msg.kind = HeadTrackingMessageKind::incompatible;
        return msg;
    }
    const auto source = read_string(data, offset);
    const std::size_t count = quaternion ? 4U : 3U;
    if (!source || !valid_source_id(*source) || data.size() - offset != 84U + (count * 4U)) {
        return std::nullopt;
    }
    msg.source_id = *source;
    auto& t = msg.timing;
    t.protocol_version = k_posebridge_protocol;
    t.instance_id = read_integer(data, offset, 8U);
    t.source_session_id = read_integer(data, offset, 8U);
    t.source_sequence = read_integer(data, offset, 8U);
    t.tx_sequence = read_integer(data, offset, 8U);
    t.reference_epoch = read_integer(data, offset, 8U);
    t.metadata_revision = read_integer(data, offset, 8U);
    t.source_received_ns = read_integer(data, offset, 8U);
    t.source_age_at_send_ns = read_integer(data, offset, 8U);
    t.sample_time_kind = static_cast<std::uint32_t>(read_integer(data, offset, 4U));
    t.sample_time_ms = read_integer(data, offset, 8U);
    t.sample_clock_epoch = read_integer(data, offset, 8U);
    const std::array positive{
        t.instance_id, t.source_session_id, t.source_sequence, t.tx_sequence, t.reference_epoch, t.metadata_revision};
    const std::array nonnegative{t.source_received_ns, t.source_age_at_send_ns, t.sample_time_ms, t.sample_clock_epoch};
    if (!std::ranges::all_of(positive, [](auto v) { return v > 0U && signed_range(v); }) ||
        !std::ranges::all_of(nonnegative, signed_range) || t.source_age_at_send_ns >= 500000000U ||
        t.sample_time_kind > 2U ||
        (t.sample_time_kind == 0U && (t.sample_time_ms != 0U || t.sample_clock_epoch != 0U)) ||
        (t.sample_time_kind != 0U && t.sample_clock_epoch == 0U)) {
        return std::nullopt;
    }
    const auto pose = orientation(data, offset, quaternion);
    if (!pose) {
        return std::nullopt;
    }
    msg.orientation = *pose;
    return msg;
}

} // namespace

bool valid_source_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 256U || std::ranges::all_of(value, [](char c) { return c == ' '; })) {
        return false;
    }
    // Validate UTF-8, including overlong encodings, surrogates, and control characters.
    bool nonblank = false;
    const auto whitespace = [](std::uint32_t c) {
        return c == 0x20U || c == 0xa0U || c == 0x1680U || (c >= 0x2000U && c <= 0x200aU) || c == 0x2028U ||
               c == 0x2029U || c == 0x202fU || c == 0x205fU || c == 0x3000U;
    };
    std::uint32_t point = 0;
    std::uint32_t minimum = 0;
    unsigned remaining = 0;
    for (const char character : value) {
        const auto c = static_cast<unsigned char>(character);
        if (remaining == 0U) {
            if (c < 0x80U) {
                if (c < 0x20U || c == 0x7fU) {
                    return false;
                }
                nonblank = nonblank || !whitespace(c);
                continue;
            }
            if (c >= 0xc2U && c <= 0xdfU) {
                remaining = 1;
                point = c & 0x1fU;
                minimum = 0x80U;
            } else if (c >= 0xe0U && c <= 0xefU) {
                remaining = 2;
                point = c & 0x0fU;
                minimum = 0x800U;
            } else if (c >= 0xf0U && c <= 0xf4U) {
                remaining = 3;
                point = c & 0x07U;
                minimum = 0x10000U;
            } else {
                return false;
            }
        } else {
            if ((c & 0xc0U) != 0x80U) {
                return false;
            }
            point = (point << 6U) | (c & 0x3fU);
            --remaining;
            if (remaining == 0U) {
                nonblank = nonblank || !whitespace(point);
            }
            if (remaining == 0U && (point < minimum || point > 0x10ffffU || (point >= 0xd800U && point <= 0xdfffU) ||
                                    (point >= 0x80U && point <= 0x9fU))) {
                return false;
            }
        }
    }
    return remaining == 0U && nonblank;
}

std::optional<HeadTrackingMessage> decode_head_tracking_osc(std::span<const std::byte> data) noexcept {
    try {
        if (data.size() > k_max_posebridge_packet) {
            return std::nullopt;
        }
        std::size_t offset = 0;
        const auto address = read_string(data, offset);
        const auto tags = read_string(data, offset);
        if (!address || !tags) {
            return std::nullopt;
        }
        if (address->starts_with("/posebridge/v")) {
            HeadTrackingMessage msg;
            msg.kind = HeadTrackingMessageKind::incompatible;
            return msg;
        }
        if (*address == "/posebridge/info" || *address == "/posebridge/status") {
            if (*tags != ",s") {
                return std::nullopt;
            }
            const auto text = read_string(data, offset);
            if (!text || offset != data.size()) {
                return std::nullopt;
            }
            return telemetry(*address, *text);
        }
        const bool quaternion = *address == "/posebridge/quaternion";
        if (!quaternion && *address != "/posebridge/euler") {
            return std::nullopt;
        }
        if (*tags != (quaternion ? ",ishhhhhhhhihhffff" : ",ishhhhhhhhihhfff")) {
            return std::nullopt;
        }
        return pose_message(data, offset, quaternion);
    } catch (...) {
        return std::nullopt;
    }
}

bool OscSourceOrder::retired(std::uint64_t instance_id) const noexcept {
    return std::ranges::find(retired_, instance_id) != retired_.end();
}
bool OscSourceOrder::accept(const HeadTrackingTiming& t) noexcept {
    const bool new_instance = t.instance_id != last_.instance_id;
    last_gap_ = 0;
    if (new_instance) {
        if (retired(t.instance_id)) {
            return false;
        }
    } else {
        if (t.tx_sequence <= last_.tx_sequence || t.metadata_revision < last_.metadata_revision ||
            t.reference_epoch < last_.reference_epoch) {
            return false;
        }
        if (t.source_session_id == last_.source_session_id) {
            if (t.source_sequence <= last_.source_sequence || t.source_received_ns < last_.source_received_ns) {
                return false;
            }
            if (t.sample_time_kind != 0U && clock_.sample_time_kind != 0U &&
                (t.sample_clock_epoch < clock_.sample_clock_epoch ||
                 (t.sample_clock_epoch == clock_.sample_clock_epoch &&
                  (t.sample_time_kind != clock_.sample_time_kind || t.sample_time_ms <= clock_.sample_time_ms)))) {
                return false;
            }
        } else if (t.metadata_revision <= last_.metadata_revision || t.reference_epoch <= last_.reference_epoch) {
            return false;
        }
        last_gap_ = t.tx_sequence - last_.tx_sequence - 1U;
    }
    if (new_instance) {
        retired_.at(retired_next_) = last_.instance_id;
        retired_next_ = (retired_next_ + 1U) % retired_.size();
    }
    if (new_instance || t.source_session_id != last_.source_session_id) {
        clock_ = {};
    }
    last_ = t;
    if (t.sample_time_kind != 0U) {
        clock_ = t;
    }
    return true;
}

} // namespace mradm::realtime
