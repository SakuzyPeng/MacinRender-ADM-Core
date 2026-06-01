#pragma once

#include <memory>

#include "adm/capability.h"
#include "adm/render.h"

namespace mradm {

// Returns the static capability report for the SAF binaural HRTF backend.
CapabilityReport binaural_capabilities();

// Creates a binaural IRenderer using SAF's built-in KEMAR HRTF dataset or a
// user-supplied FIR SOFA HRIR file. Objects and DirectSpeakers content is
// rendered via VBAP-interpolated HRTF convolution. HOA tracks are skipped.
// Output is always 2-channel (L/R binaural); output_layout is ignored.
std::unique_ptr<IRenderer> create_binaural_renderer();

// Whether this build can load user SOFA HRIR files (MR_ADM_ENABLE_SOFA=ON, which
// enables SAF's SOFA reader). When false, --sofa / adm_render_options_set_sofa_path
// is rejected at render time and only the built-in KEMAR HRTF is available.
bool binaural_sofa_supported();

} // namespace mradm
