#pragma once

#include <string>

#include "adm/scene.h"

namespace mradm::engine {

// Serialize a full AdmScene tree to a JSON string (UTF-8). The schema mirrors
// the `mradm inspect` field set. Optional fields are omitted when unset; an
// end_sample of UINT64_MAX (meaning "to end of file") is omitted rather than
// emitted as a sentinel. nlohmann/json is used purely inside this TU — no
// third-party type appears in this declaration.
[[nodiscard]] std::string scene_to_json(const AdmScene& scene);

} // namespace mradm::engine
