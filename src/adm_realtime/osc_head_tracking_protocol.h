#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "adm/head_tracking.h"

namespace mradm::realtime {

struct HeadTrackingMessage {
    HeadTrackingOrientation orientation;
    HeadTrackingTiming timing;
};

// Bounded replay/ordering memory for a single sender. v1 never clears v2 history.
// Not authentication; the most recent 16 retired source sessions are remembered.
class OscSourceOrder {
  public:
    [[nodiscard]] bool accept(const HeadTrackingTiming& timing) noexcept;

  private:
    HeadTrackingTiming last_source_;
    HeadTrackingTiming last_clock_;
    std::array<std::uint64_t, 16> retired_{};
    std::size_t retired_next_{0};
};

// One exact OSC message per datagram. No allocation, bundles, partial-axis state or timetags.
[[nodiscard]] std::optional<HeadTrackingMessage> decode_head_tracking_osc(std::span<const std::byte> data) noexcept;

} // namespace mradm::realtime
