#pragma once

#include <memory>

#include "adm/renderer.h"

namespace mradm {

// The macOS AUSpatialMixer backend (src/adm_apple/apple_backend.h) as a C++ IRenderer for the
// tests; its streams are the monitor's island table behind an IRenderStream.
[[nodiscard]] std::unique_ptr<IRenderer> create_apple_renderer();

} // namespace mradm
