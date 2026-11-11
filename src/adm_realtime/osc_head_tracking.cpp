#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "adm/head_tracking.h"

#include "osc_ffi.h"

// The receiver is Rust mradm-osc `receiver` (socket, worker, ordering, snapshot and its JSON);
// this class keeps the C++ interface for the C ABI.
namespace mradm {
namespace {

static_assert(sizeof(MradmOscTiming) == sizeof(HeadTrackingTiming));

struct TextDeleter {
    void operator()(MradmOscText* text) const noexcept { mradm_osc_text_destroy(text); }
};

std::string take(MradmOscText* raw) {
    const std::unique_ptr<MradmOscText, TextDeleter> text{raw};
    const std::uint8_t* data = nullptr;
    const std::size_t size = mradm_osc_text_view(text.get(), &data);
    return size == 0U ? std::string{} : std::string(reinterpret_cast<const char*>(data), size);
}

} // namespace

struct OscHeadTrackingReceiver::Impl {
    explicit Impl(MradmOscReceiver* receiver) noexcept : handle_(receiver) {}
    ~Impl() { mradm_osc_receiver_destroy(handle_); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    [[nodiscard]] MradmOscReceiver* get() const noexcept { return handle_; }

  private:
    MradmOscReceiver* handle_;
};

OscHeadTrackingReceiver::OscHeadTrackingReceiver(std::uint16_t port, std::string source_id) {
    auto* handle =
        mradm_osc_receiver_create(port, reinterpret_cast<const std::uint8_t*>(source_id.data()), source_id.size());
    if (handle == nullptr) {
        throw std::bad_alloc();
    }
    impl_ = std::make_unique<Impl>(handle);
}

OscHeadTrackingReceiver::~OscHeadTrackingReceiver() = default;

Result<void> OscHeadTrackingReceiver::start() {
    MradmOscText* message = nullptr;
    const auto code = mradm_osc_receiver_start(impl_->get(), &message);
    auto text = take(message);
    if (code == 0) {
        return {};
    }
    return make_error(static_cast<ErrorCode>(code), std::move(text));
}

void OscHeadTrackingReceiver::stop() noexcept {
    mradm_osc_receiver_stop(impl_->get());
}

OscHeadTrackingSnapshot OscHeadTrackingReceiver::snapshot() const {
    MradmOscSnapshot raw{};
    if (mradm_osc_receiver_snapshot(impl_->get(), &raw) != 0) {
        throw std::runtime_error("OSC 快照读取失败");
    }
    OscHeadTrackingSnapshot result;
    result.state = static_cast<OscHeadTrackingState>(raw.state);
    result.bound_port = static_cast<std::uint16_t>(raw.bound_port);
    result.has_pose = raw.has_pose != 0U;
    result.fresh = raw.fresh != 0U;
    result.session_id = raw.session_id;
    result.sequence = raw.sequence;
    result.received_ns = raw.received_ns;
    result.age_ms = raw.age_ms;
    result.packets_received = raw.packets_received;
    result.rejected_packets = raw.rejected_packets;
    result.recovery_count = raw.recovery_count;
    result.pose_packets = raw.pose_packets;
    result.telemetry_packets = raw.telemetry_packets;
    result.ignored_sources = raw.ignored_sources;
    result.protocol_mismatches = raw.protocol_mismatches;
    result.missing_tx_packets = raw.missing_tx_packets;
    result.has_heartbeat = raw.has_heartbeat != 0U;
    result.heartbeat_alive = raw.heartbeat_alive != 0U;
    result.heartbeat_age_ms = raw.heartbeat_age_ms;
    result.source_id.assign(reinterpret_cast<const char*>(raw.source_id),
                            std::min(raw.source_id_len, sizeof(raw.source_id)));
    result.info_matches_pose = raw.info_matches_pose != 0U;
    result.status_matches_pose = raw.status_matches_pose != 0U;
    std::ranges::copy(raw.quaternion_xyzw, result.orientation.quaternion_xyzw.begin());
    std::ranges::copy(raw.euler_deg, result.orientation.euler_deg.begin());
    result.timing = std::bit_cast<HeadTrackingTiming>(raw.timing); // same field order (osc_ffi.h)
    return result;
}

std::string OscHeadTrackingReceiver::snapshot_json() const {
    MradmOscText* text = nullptr;
    const auto code = mradm_osc_receiver_snapshot_json(impl_->get(), &text);
    auto json = take(text);
    if (code != 0) {
        throw std::runtime_error(json);
    }
    return json;
}

std::string OscHeadTrackingReceiver::last_error() const {
    MradmOscText* text = nullptr;
    if (mradm_osc_receiver_last_error(impl_->get(), &text) != 0) {
        throw std::runtime_error("OSC 错误信息查询失败");
    }
    return take(text);
}

} // namespace mradm
