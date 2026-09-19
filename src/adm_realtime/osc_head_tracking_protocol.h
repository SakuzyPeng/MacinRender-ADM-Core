#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "adm/head_tracking.h"

namespace mradm::realtime {

// One exact OSC message per datagram. No allocation, bundles, partial-axis state or timetags.
[[nodiscard]] std::optional<HeadTrackingOrientation> decode_head_tracking_osc(std::span<const std::byte> data) noexcept;

} // namespace mradm::realtime
