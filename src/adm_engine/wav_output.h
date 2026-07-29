#pragma once

#include <stop_token>
#include <string>

#include "adm/errors.h"
#include "adm/progress.h"

namespace mradm::engine {

// Add the machine-readable channel semantics required by the selected layout.
// Mask-representable speaker layouts use WAVEFORMATEXTENSIBLE; binaural, HOA,
// and speaker layouts that exceed the WAVE mask vocabulary use ADM AXML/CHNA.
Result<void> finalize_rendered_wav(const std::string& path,
                                   const std::string& output_layout,
                                   const std::stop_token& cancel_token = {},
                                   ProgressSink* progress = nullptr);

} // namespace mradm::engine
