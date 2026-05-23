#pragma once

#include <memory>

#include "adm/capability.h"
#include "adm/render.h"

namespace mradm {

// Returns the static capability report for the SAF binaural HRTF backend.
CapabilityReport binaural_capabilities();

// Creates a binaural IRenderer using SAF's built-in KEMAR HRTF dataset (836
// directions, 256-tap FIR @ 48 kHz).  Objects and DirectSpeakers content is
// rendered via VBAP-interpolated HRTF convolution.  HOA tracks are skipped.
// Output is always 2-channel (L/R binaural); output_layout is ignored.
std::unique_ptr<IRenderer> create_binaural_renderer();

} // namespace mradm
