#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "adm/audio_io.h"

namespace mradm::audio {

std::string compact_metadata_comment(const MetadataFields& meta);

Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_caf_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_flac_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_mka_metadata(const std::string& path, const MetadataFields& meta);

// Layout-aware finalization for a rendered WAV. The renderer writes its native
// interleaved order first; this rewrite is deliberately the last audio-domain
// operation so channel permutations cannot be undone by trim/gain/bit-depth
// processing.
struct WavChnaEntry {
    uint16_t track_index{0}; // one-based
    std::string track_uid;
    std::string track_format;
    std::string pack_format;
};

struct WavLayoutFinalization {
    uint32_t channel_mask{0};
    // File channel -> source/render channel. Empty means identity.
    std::vector<uint16_t> output_channel_sources;
    // Non-empty AXML/CHNA turns the file into an ADM-labelled output. Integer
    // PCM is written as BW64; float32 remains RF64 with ADM chunks because
    // ITU-R BS.2088 does not define IEEE-float BW64 as a normative format.
    std::string axml;
    std::vector<WavChnaEntry> chna;
};

Result<void> finalize_wav_layout(const std::string& path,
                                 const WavLayoutFinalization& layout,
                                 const std::stop_token& cancel_token = {},
                                 ProgressSink* progress = nullptr,
                                 RenderOperation operation = RenderOperation::write_metadata);

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
