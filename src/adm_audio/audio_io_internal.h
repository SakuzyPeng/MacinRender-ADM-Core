#pragma once

#include <cstdint>
#include <string>

#include "adm/audio_io.h"

namespace mradm::audio {

std::string compact_metadata_comment(const MetadataFields& meta);

Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_caf_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_flac_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_mka_metadata(const std::string& path, const MetadataFields& meta);

// Subprocess worker behind convert_to_apac's stall watchdog. Runs the real
// AudioToolbox encode in-process and streams a line-based heartbeat protocol on
// stdout so a parent process can distinguish "slow but progressing" from a
// spin-hang and reclaim the wedged encoder by killing this process:
//   "P <done> <total>\n"  after each block write (write-phase heartbeat)
//   "F\n"                 just before ExtAudioFileDispose (flush phase begins)
//   "E <code> <message>\n" on failure (code = mradm::ErrorCode int), then exit !=0
// Reached only via the hidden `mradm __apac-encode` command (RenderService bridge);
// internal mechanism, deliberately kept off the public audio_io.h surface.
// Returns ErrorCode::unsupported on non-Apple platforms.
Result<void> run_apac_encode_child(const std::string& src_path,
                                   const std::string& apac_path,
                                   const std::string& layout_id,
                                   uint32_t bitrate_kbps,
                                   bool drc_music,
                                   bool caf_container);

} // namespace mradm::audio
