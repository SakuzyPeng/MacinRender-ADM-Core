#pragma once

#include <string>

#include "adm/io.h"

namespace mradm::io {

// Offline-render import: import_scene, plus the render-scene checkpoint `05-import` written by the
// Rust scene in diagnostic builds (PCM consistency 4.3). Other import callers record no checkpoint.
Result<AdmScene> import_scene_for_render(const std::string& path, const SceneImportOptions& options);

} // namespace mradm::io
