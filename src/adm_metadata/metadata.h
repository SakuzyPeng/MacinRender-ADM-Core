#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/scene.h"

#include "audio_io_internal.h"
#include "speaker_layouts.h"

namespace mradm::metadata {
struct OutputMetadata {
    std::string axml;
    std::vector<audio::WavChnaEntry> chna;
};
Result<AdmScene> import_axml(std::string_view xml, const std::map<std::string, uint16_t>& channels, uint32_t rate);
Result<std::string> patch_axml(std::string_view xml, const AdmScene& original, const AdmScene& effective);
Result<OutputMetadata>
generate(uint32_t kind, std::string_view name, const std::vector<render_layouts::SpeakerSpec>& speakers);
} // namespace mradm::metadata
