#pragma once

#include <string>

#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm::io {

// Import an ADM BWF file from path and return a self-owned AdmScene.
// Returns io_error if the file is missing, not a valid BW64/ADM file,
// or the axml chunk is absent.
Result<AdmScene> import_scene(const std::string& path);

// Return the raw AXML chunk content as a UTF-8 string without parsing.
Result<std::string> get_axml(const std::string& path);

} // namespace mradm::io
