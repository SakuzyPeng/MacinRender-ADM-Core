#pragma once

#include <string>

namespace mradm::engine {

// Serialize the available renderer backends and their capabilities to a JSON
// string (UTF-8). The schema mirrors the `mradm backends` field set: per-backend
// feature flags plus the supported output layouts. nlohmann/json is used purely
// inside this TU — no third-party type appears in this declaration.
[[nodiscard]] std::string capabilities_to_json();

} // namespace mradm::engine
