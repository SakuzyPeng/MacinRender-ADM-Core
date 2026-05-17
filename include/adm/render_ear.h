#pragma once

#include <memory>

#include "adm/capability.h"
#include "adm/render.h"

namespace mradm {

// Returns the static capability report for the libear backend.
CapabilityReport ear_capabilities();

// Creates a libear-based IRenderer for Objects-type ADM content.
std::unique_ptr<IRenderer> create_ear_renderer();

} // namespace mradm
