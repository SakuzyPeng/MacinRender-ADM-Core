#pragma once

#include <string>

namespace mradm::engine {

// The output container-format reference, serialized to a JSON string (UTF-8).
// Describes every output container the engine can produce: its file extensions,
// whether it is available in this build / on this platform (with a human-readable
// reason when not), and its constraints (channel limit, fixed sample rate, height
// support, bit depths, bitrate ranges). Also carries a "features" object with the
// build/platform feature flags (apac / iamf / iamf_mp4_packager / sofa) so a GUI
// can gray out unavailable options. nlohmann/json is used purely inside the TU —
// no third-party type appears in this declaration.
//
// Does no project-file I/O. In IAMF-enabled builds the "iamf_mp4_packager" flag
// probes PATH for an MP4 packager (and may spawn a short-lived mp4box/ffmpeg
// subprocess); in the default MR_ADM_ENABLE_IAMF=OFF build that flag is false and
// no probe runs.
[[nodiscard]] std::string output_formats_to_json();

} // namespace mradm::engine
