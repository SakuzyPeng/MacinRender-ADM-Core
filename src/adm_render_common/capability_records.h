#pragma once

#include <cstdint>

#include "adm/capability.h"

namespace mradm::render_common {

// The Rust backends in discovery order (mradm-engine `catalog`, PCM consistency 5.2 A).
enum class EngineBackend : uint32_t { ear = 0, saf = 1, triple_balance = 2, hoa = 3, binaural = 4, room = 5 };

// The capability report of a Rust backend, copied from its Rust record.
[[nodiscard]] CapabilityReport engine_capabilities(EngineBackend backend);

} // namespace mradm::render_common
