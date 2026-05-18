#pragma once

#include <string>

#include "adm/errors.h"
#include "adm/logging.h"

namespace mradm {

// Measure the integrated loudness (ITU-R BS.1770-4 / EBU R128) of a rendered
// WAV file and apply a single global gain to reach target_lufs.  No-op when
// the measured loudness is already within 0.1 LU of the target, or when the
// signal is silence / too short for the gating algorithm to produce a valid
// measurement.  Two-pass: measure then (if needed) rewrite in-place.
Result<void> apply_loudness_norm(const std::string& path, float target_lufs, LogSink& logs);

} // namespace mradm
