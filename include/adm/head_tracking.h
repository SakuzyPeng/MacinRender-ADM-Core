#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "adm/errors.h"

namespace mradm {

// PoseBridge v1/v2 / GUI convention: Hamilton x,y,z,w; q = q_y(yaw) q_x(pitch) q_z(roll).
// These quaternion axes are NOT the renderer's scene-space axes. Submit euler_deg to
// the existing listener-orientation APIs: yaw left, pitch up, roll right, in degrees.
struct HeadTrackingOrientation {
    std::array<float, 4> quaternion_xyzw{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> euler_deg{}; // yaw, pitch, roll
};

enum class OscHeadTrackingState : std::int32_t {
    idle = 0,
    waiting = 1,
    active = 2,
    stale = 3,
    stopped = 4,
    failed = 5
};

// Clock domains are independent; timestamps must not be subtracted across them.
struct HeadTrackingTiming {
    std::uint32_t protocol_version{0}; // 0 before data, 1 pose-only, 2 metadata
    std::uint32_t sample_time_kind{0}; // 0 absent, 1 device calendar (NOT UTC), 2 simulated elapsed
    std::uint64_t source_session_id{0};
    std::uint64_t source_sequence{0};
    std::uint64_t source_received_ns{0}; // since PoseBridge source session start
    std::uint64_t sample_time_ms{0};     // device clock since 2000-01-01, or simulated elapsed
    std::uint64_t sample_clock_epoch{0}; // nonzero if sample time present; changes on clock discontinuity
};

struct OscHeadTrackingSnapshot {
    OscHeadTrackingState state{OscHeadTrackingState::idle};
    std::uint16_t bound_port{0};
    bool has_pose{false};
    bool fresh{false};
    std::uint64_t session_id{0};
    std::uint64_t sequence{0};
    std::uint64_t received_ns{0}; // monotonic time since this start; not device sampling time
    std::uint64_t age_ms{0};      // meaningful only when has_pose
    std::uint64_t packets_received{0};
    std::uint64_t rejected_packets{0};
    std::uint64_t recovery_count{0}; // valid input after >=500 ms silence; caller should check recenter
    HeadTrackingOrientation orientation;
    HeadTrackingTiming timing;
};

// Independent, loopback-only UDP receiver. Implemented in ADMRealtime; no audio device,
// GUI, BLE dependency or callback. The worker only replaces a single latest snapshot.
// Poll on a control thread, then apply recenter/smoothing/arbitration in the host and
// submit to Monitor/Scene using their existing thread/lifetime contracts.
// Serialize start/stop/destruction; snapshot/error queries may run alongside start/stop,
// but no call may overlap destruction. stop joins the worker; not an audio-callback API.
class OscHeadTrackingReceiver {
  public:
    // port 0 requests an OS-assigned port for embedding/tests; default is 9000.
    explicit OscHeadTrackingReceiver(std::uint16_t port = 9000);
    ~OscHeadTrackingReceiver();
    OscHeadTrackingReceiver(const OscHeadTrackingReceiver&) = delete;
    OscHeadTrackingReceiver& operator=(const OscHeadTrackingReceiver&) = delete;
    OscHeadTrackingReceiver(OscHeadTrackingReceiver&&) = delete;
    OscHeadTrackingReceiver& operator=(OscHeadTrackingReceiver&&) = delete;

    [[nodiscard]] Result<void> start(); // synchronous bind; idempotent while running
    void stop() noexcept;               // idempotent; retains the last pose with fresh=false
    [[nodiscard]] OscHeadTrackingSnapshot snapshot() const;
    [[nodiscard]] std::string last_error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mradm
