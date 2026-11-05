#pragma once

#include <string>

// The output-layout aliases of the Rust request rules (mradm-engine `request`), for the monitor's
// system-spatial sink selection. Backend selection, validation and preparation are Rust, shared by
// the offline render and the monitor so the two never diverge (PCM consistency 5.2 B, 5.3 B). See
// docs/architecture/REALTIME_MONITORING_SLICE1.md §5.
namespace mradm {

// Map a requested output-layout string (case-insensitive aliases like "5.1" / "atmos714" /
// "stereo") to its canonical layout id ("0+5+0" / "4+7+0" / "0+2+0" / "binaural" / "hoa3"…).
// Unknown values pass through unchanged. Shared by RenderService and the realtime monitor so
// both accept the same aliases.
[[nodiscard]] std::string normalize_output_layout(const std::string& layout);

} // namespace mradm
