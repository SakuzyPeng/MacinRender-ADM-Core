#include "osc_head_tracking_protocol.h"

#include <algorithm>
#include <cstdint>

namespace mradm::realtime {
namespace {

MradmOscTiming to_rust(const HeadTrackingTiming& t) noexcept {
    return MradmOscTiming{t.protocol_version,
                          t.sample_time_kind,
                          t.instance_id,
                          t.tx_sequence,
                          t.reference_epoch,
                          t.metadata_revision,
                          t.source_age_at_send_ns,
                          t.source_session_id,
                          t.source_sequence,
                          t.source_received_ns,
                          t.sample_time_ms,
                          t.sample_clock_epoch};
}

HeadTrackingTiming from_rust(const MradmOscTiming& t) noexcept {
    HeadTrackingTiming result;
    result.protocol_version = t.protocol_version;
    result.sample_time_kind = t.sample_time_kind;
    result.instance_id = t.instance_id;
    result.tx_sequence = t.tx_sequence;
    result.reference_epoch = t.reference_epoch;
    result.metadata_revision = t.metadata_revision;
    result.source_age_at_send_ns = t.source_age_at_send_ns;
    result.source_session_id = t.source_session_id;
    result.source_sequence = t.source_sequence;
    result.source_received_ns = t.source_received_ns;
    result.sample_time_ms = t.sample_time_ms;
    result.sample_clock_epoch = t.sample_clock_epoch;
    return result;
}

const std::uint8_t* bytes(const void* data) noexcept {
    return static_cast<const std::uint8_t*>(data);
}

} // namespace

bool valid_source_id(std::string_view value) noexcept {
    return mradm_osc_valid_source_id(bytes(value.data()), value.size()) != 0U;
}

std::optional<HeadTrackingMessage> decode_head_tracking_osc(std::span<const std::byte> data) noexcept {
    try {
        MradmOscMessage raw{};
        if (mradm_osc_decode(bytes(data.data()), data.size(), &raw) != 0 || raw.kind > 3U ||
            raw.source_id_len > sizeof(raw.source_id) || raw.json_offset > data.size() ||
            raw.json_len > data.size() - raw.json_offset) {
            return std::nullopt;
        }
        HeadTrackingMessage msg;
        msg.kind = static_cast<HeadTrackingMessageKind>(raw.kind);
        std::ranges::copy(raw.quaternion_xyzw, msg.orientation.quaternion_xyzw.begin());
        std::ranges::copy(raw.euler_deg, msg.orientation.euler_deg.begin());
        msg.timing = from_rust(raw.timing);
        msg.source_id.assign(reinterpret_cast<const char*>(raw.source_id), raw.source_id_len);
        msg.message_sequence = raw.message_sequence;
        msg.reported_samples = raw.reported_samples;
        msg.source_active = raw.source_active != 0U;
        msg.json.assign(reinterpret_cast<const char*>(data.data()) + raw.json_offset, raw.json_len);
        return msg;
    } catch (...) {
        return std::nullopt;
    }
}

bool OscSourceOrder::retired(std::uint64_t instance_id) const noexcept {
    return mradm_osc_source_order_retired(&state_, instance_id) != 0U;
}

bool OscSourceOrder::accept(const HeadTrackingTiming& t) noexcept {
    const auto timing = to_rust(t);
    return mradm_osc_source_order_accept(&state_, &timing) != 0U;
}

} // namespace mradm::realtime
