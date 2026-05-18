#pragma once

#include <string>

#include "adm/errors.h"
#include "adm/logging.h"

namespace mradm {

// Measure the True Peak (ITU-R BS.1770-4) of a rendered WAV file and apply a
// single global gain if it exceeds target_dbtp. No-op when True Peak is already
// at or below the target. Two-pass: measure then (if needed) rewrite in-place.
Result<void> apply_peak_limit(const std::string& path, float target_dbtp, LogSink& logs);

} // namespace mradm
