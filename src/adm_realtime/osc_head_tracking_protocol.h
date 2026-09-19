#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "adm/head_tracking.h"

namespace mradm::realtime {

inline constexpr std::uint32_t k_posebridge_protocol = 3;
inline constexpr std::size_t k_max_posebridge_packet = 8192;
enum class HeadTrackingMessageKind { pose, info, status, incompatible };
struct HeadTrackingMessage {
    HeadTrackingMessageKind kind{HeadTrackingMessageKind::pose};
    HeadTrackingOrientation orientation;
    HeadTrackingTiming timing;
    std::string source_id;
    std::uint64_t message_sequence{0};
    std::uint64_t reported_samples{0};
    bool source_active{false};
    std::string json;
};

// Ordering applies to accepted poses only; telemetry cannot retire a pose stream.
class OscSourceOrder {
  public:
    [[nodiscard]] bool accept(const HeadTrackingTiming& timing) noexcept;
    [[nodiscard]] bool retired(std::uint64_t instance_id) const noexcept;
    [[nodiscard]] std::uint64_t last_gap() const noexcept { return last_gap_; }

  private:
    HeadTrackingTiming last_;
    HeadTrackingTiming clock_;
    std::array<std::uint64_t, 16> retired_{};
    std::size_t retired_next_{0};
    std::uint64_t last_gap_{0};
};

[[nodiscard]] bool valid_source_id(std::string_view value) noexcept;
// Current protocol only, one complete message per datagram, at most 8 KiB.
// Strings/JSON may allocate on the background receiver; never call in an audio callback.
[[nodiscard]] std::optional<HeadTrackingMessage> decode_head_tracking_osc(std::span<const std::byte> data) noexcept;

} // namespace mradm::realtime
