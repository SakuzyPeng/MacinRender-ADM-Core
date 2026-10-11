#pragma once
#include "adm/render.h"
#include "adm/room.h"

namespace mradm {
[[nodiscard]] CapabilityReport room_capabilities();
[[nodiscard]] std::unique_ptr<IRenderer> create_room_renderer();
} // namespace mradm
