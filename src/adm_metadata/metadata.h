#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/scene.h"

#include "audio_io_internal.h"
#include "speaker_layouts.h"

struct MradmAdmView;

namespace mradm::metadata {
struct OutputMetadata {
    std::string axml;
    std::vector<audio::WavChnaEntry> chna;
};
// The public scene of a borrowed record view (scene_ffi.h / adm_ffi.h). Without ADM provenance
// (a synthesized channel bed) the source records are not read and adm_source stays empty.
Result<AdmScene> scene_from_view(const MradmAdmView& view, bool adm_provenance);
Result<std::string> patch_axml(std::string_view xml, const AdmScene& original, const AdmScene& effective);
Result<OutputMetadata>
generate(uint32_t kind, std::string_view name, const std::vector<render_layouts::SpeakerSpec>& speakers);
} // namespace mradm::metadata
