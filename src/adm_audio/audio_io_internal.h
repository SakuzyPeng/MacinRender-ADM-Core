#pragma once

#include <string>

#include "adm/audio_io.h"

namespace mradm::audio {

std::string compact_metadata_comment(const MetadataFields& meta);

Result<void> write_wav_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_caf_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_flac_metadata(const std::string& path, const MetadataFields& meta);
Result<void> write_mka_metadata(const std::string& path, const MetadataFields& meta);

} // namespace mradm::audio
