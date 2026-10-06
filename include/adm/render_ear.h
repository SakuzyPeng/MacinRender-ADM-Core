#pragma once

#include <memory>

#include "adm/capability.h"
#include "adm/render.h"

namespace mradm {

// Returns the static capability report for the Rust EAR backend.
CapabilityReport ear_capabilities();

// Creates a Rust EAR IRenderer for Objects, DirectSpeakers and HOA content.
std::unique_ptr<IRenderer> create_ear_renderer();

} // namespace mradm
