#pragma once

#include <string>

#include "adm/render.h"

namespace mradm::engine {

// The output container-format reference: every output container the engine can
// produce, its file extensions, whether it is available in this build / on this
// platform (with a human-readable reason when not), and its constraints (channel
// limit, fixed sample rate, height support, bit depths, bitrate ranges), plus the
// build/platform feature flags. Single source of truth shared by the CLI
// (`mradm formats`) and the C ABI (adm_output_formats_json) so they never drift.
//
// Does no project-file I/O. In IAMF-enabled builds the "iamf_mp4_packager" flag
// probes PATH for an MP4 packager (and may spawn a short-lived mp4box/ffmpeg
// subprocess); in the default MR_ADM_ENABLE_IAMF=OFF build that flag is false and
// no probe runs.
[[nodiscard]] OutputFormats build_output_formats();

// The same reference serialized to a JSON string (UTF-8). nlohmann/json is used
// purely inside the TU — no third-party type appears in this declaration.
[[nodiscard]] std::string output_formats_to_json();

} // namespace mradm::engine
