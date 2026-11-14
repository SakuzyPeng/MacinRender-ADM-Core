#pragma once

#include <memory>

#include "adm/capability.h"
#include "adm/render.h"

namespace mradm {

// Returns the static capability report for the macOS AUSpatialMixer backend.
CapabilityReport apple_capabilities();

// macOS runtime self-test: true if this OS's AUSpatialMixer routes a bypass-LFE (LFEScreen-labelled)
// source to the output LFE channel. macOS ≤26.3 misroutes it to Center (verified: 26.3 broken,
// 26.5.1+ fixed), so an Apple-rendered system-spatial bed drops/misplaces the LFE there. Probed once
// (instantiate AUSpatialMixer + render an LFE bus) and cached; lets the UI steer system-spatial
// monitoring to an EAR/VBAP bed on affected systems. Returns true if the probe can't run (no false
// alarms). Defined only in the macOS-gated apple target.
[[nodiscard]] bool apple_system_spatial_lfe_routing_ok();

// Creates a macOS-only IRenderer backed by Apple AUSpatialMixer.
std::unique_ptr<IRenderer> create_apple_renderer();

} // namespace mradm
