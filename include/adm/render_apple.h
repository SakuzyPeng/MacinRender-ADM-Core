#pragma once

#include <memory>

#include "adm/capability.h"
#include "adm/render.h"

namespace mradm {

// Returns the static capability report for the macOS AUSpatialMixer backend.
CapabilityReport apple_capabilities();

// Creates a macOS-only IRenderer backed by Apple AUSpatialMixer.
std::unique_ptr<IRenderer> create_apple_renderer();

} // namespace mradm
