#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "adm/errors.h"

namespace mradm {
namespace room_internal {
struct Access;
}

// Loaded immutable room assets. Personal HRIR preparation is separate and reusable.
class RoomProfile final {
  public:
    static Result<std::shared_ptr<const RoomProfile>> load(const std::filesystem::path& manifest);
    ~RoomProfile();
    RoomProfile(const RoomProfile&) = delete;
    RoomProfile& operator=(const RoomProfile&) = delete;
    RoomProfile(RoomProfile&&) = delete;
    RoomProfile& operator=(RoomProfile&&) = delete;
    [[nodiscard]] std::string manifest_json() const;

  private:
    friend struct room_internal::Access;
    explicit RoomProfile(void* handle) : handle_(handle) {}
    void* handle_;
};

// Fixed for a session. Existing callers retain the deterministic exact path.
enum class RoomQuality : std::uint32_t { exact = 0, realtime = 1 };
[[nodiscard]] constexpr const char* room_quality_name(RoomQuality quality) {
    switch (quality) {
    case RoomQuality::exact:
        return "exact";
    case RoomQuality::realtime:
        return "realtime";
    }
    return "unknown";
}

// Public snapshots are consumed in other translation units and language bindings.
// cppcheck-suppress-begin unusedStructMember
struct RoomConfig {
    std::shared_ptr<const RoomProfile> profile;
    std::filesystem::path
        hrir_manifest; // SOFA, prepared HRIR or distance collection; empty selects SOFA/package default
    double metres_per_unit{1.5};
    RoomQuality quality{RoomQuality::exact};
};

enum class RoomSwitchState : std::uint32_t { applied, preparing, transitioning, draining, failed };
struct RoomStatus {
    RoomSwitchState state{RoomSwitchState::applied};
    std::uint64_t requested_revision{0};
    std::uint64_t applied_revision{0};
    std::uint64_t transition_sample{0};
    std::uint64_t material_frames{0};
    std::uint64_t tail_frames{0};
    std::uint64_t buffer_frames{256};
    std::uint64_t material_filter_guard_frames{0};
    std::uint32_t constrained_sources{0};
    std::string message;
    RoomQuality quality{RoomQuality::exact};
    std::uint32_t reflection_update_frames{0};
};
struct RoomSourcePosition {
    std::uint64_t element_id{0};
    std::array<double, 3> position_metres{};
    std::array<double, 3> direction{};
    double distance_metres{0};
    bool constrained{false};
    bool present{false};
};
// cppcheck-suppress-end unusedStructMember

// Control-thread interface; preparation and disposal run outside the render worker.
class IRoomControl {
  public:
    virtual ~IRoomControl() = default;
    [[nodiscard]] virtual Result<std::uint64_t> request(const RoomConfig& config) = 0;
    [[nodiscard]] virtual RoomStatus status() const = 0;
    [[nodiscard]] virtual RoomSourcePosition source_position(std::uint64_t id) const = 0;
};

// Fully validates/loads room + personal HRIR and returns their prepared resource report.
[[nodiscard]] Result<std::string> inspect_room(const RoomConfig& config);
} // namespace mradm
