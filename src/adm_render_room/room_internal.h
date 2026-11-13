#pragma once
#include <array>
#include <memory>
#include <string>

#include "adm/options.h"
#include "adm/room.h"

#include "routing_ffi.h"

namespace mradm::room_internal {
struct Access {
    static const void* get(const RoomProfile& profile) { return profile.handle_; }
    // Owns a profile handle from the Rust C ABI's rooms; nullptr for nullptr.
    static std::shared_ptr<const RoomProfile> adopt(void* handle) {
        if (handle == nullptr) {
            return nullptr;
        }
        try {
            return std::shared_ptr<const RoomProfile>{new RoomProfile(handle)};
        } catch (...) {
            mradm_room_profile_destroy(handle);
            throw;
        }
    }
};
using PreparedHandle = std::unique_ptr<void, decltype(&mradm_room_prepared_destroy)>;
// cppcheck-suppress-begin unusedStructMember
struct Prepared {
    PreparedHandle handle{nullptr, mradm_room_prepared_destroy};
    MradmRoomPreparedInfo info{};
    std::string json;
};
[[nodiscard]] Result<std::shared_ptr<const Prepared>>
prepare(const RoomConfig& config, const std::filesystem::path& sofa = {}, const Prepared* reuse = nullptr);
[[nodiscard]] MradmRoomScreenGeometry geometry(const ScreenGeometry& screen);

// A RoomConfig as a Rust record; borrows the UTF-8 HRIR path it holds.
struct ConfigRecord {
    explicit ConfigRecord(const RoomConfig& config);
    std::string hrir;
    MradmRenderRoomConfig record{};
};
// IRoomControl over a Rust mradm-render `room_runtime` control handle.
[[nodiscard]] std::shared_ptr<IRoomControl> control(MradmRenderRoomControl* raw);
// cppcheck-suppress-end unusedStructMember
} // namespace mradm::room_internal
