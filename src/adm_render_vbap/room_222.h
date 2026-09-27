#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm::room_compat {
inline constexpr std::size_t k_room_222_channels = 24;
using Room222Gains = std::array<float, k_room_222_channels>;

struct Room222Node {
    std::size_t channel{};
    std::string_view label;
    float x{};
    float y{};
    float z{};
    int filter{-1};
    float sign{};
};

[[nodiscard]] bool is_room_222(std::string_view layout) noexcept;
[[nodiscard]] const std::vector<Room222Node>& room_222_nodes();
// Self-defined three-layer room extension. XYZ spans [-1,1].
[[nodiscard]] Result<Room222Gains> room_222_gains(const SceneBlockPosition& position, float size = 0);
struct Room222Mix {
    float direct{1};
    Room222Gains spread{};
};
[[nodiscard]] Room222Mix room_222_mix(const Room222Gains& spatial, float size);
} // namespace mradm::room_compat
