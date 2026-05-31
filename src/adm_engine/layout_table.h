#pragma once

#include <string>
#include <vector>

#include "adm/render.h"

namespace mradm::engine {

// The canonical output channel-order reference table, with per-row supported_by
// computed from the renderer capabilities. Single source of truth shared by the
// CLI (`mradm layouts`) and the C ABI (adm_layouts_json).
[[nodiscard]] std::vector<OutputLayoutRow> build_output_layouts();

// The same table serialized to a JSON string (UTF-8). nlohmann/json is used
// purely inside the TU — no third-party type appears in this declaration.
[[nodiscard]] std::string layouts_to_json();

} // namespace mradm::engine
