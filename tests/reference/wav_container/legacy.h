#pragma once
// Frozen C++ WAVE container metadata implementation (see legacy.cpp and retention.json); tests only.
#include <cstdint>
#include <map>
#include <stop_token>
#include <string>

#include "adm/audio_io.h"
#include "adm/errors.h"

#include "audio_io_internal.h"

namespace mradm::wav_container_legacy {

struct AdmChunks {
    std::string axml;
    std::map<std::string, uint16_t> uid_to_channel;
};

enum class AxmlState : uint8_t { absent, present, invalid_wave };

Result<void> finalize_wav_layout(const std::string& path,
                                 const audio::WavLayoutFinalization& layout,
                                 const std::stop_token& cancel_token = {});
Result<void> write_wav_metadata(const std::string& path, const audio::MetadataFields& meta);
Result<AdmChunks> read_wave_adm_metadata(const std::string& path);
Result<void>
rewrite_bwf_replacing_axml(const std::string& src_path, const std::string& dst_path, const std::string& new_axml);
AxmlState scan_axml(const std::string& path);

} // namespace mradm::wav_container_legacy
