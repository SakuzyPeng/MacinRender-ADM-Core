#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/scene.h"

#include "speaker_layouts.h"

struct MradmAdmView;
struct MradmSceneOverlay;

namespace mradm::metadata {
// The public scene of a borrowed record view (scene_ffi.h / adm_ffi.h). Without ADM provenance
// (a synthesized channel bed) the source records are not read and adm_source stays empty. A render
// scene's overlay supplies the semantic-policy state the records do not carry (user levels, the
// effective object mute and DirectSpeakers position flags).
Result<AdmScene>
scene_from_view(const MradmAdmView& view, bool adm_provenance, const MradmSceneOverlay* overlay = nullptr);
} // namespace mradm::metadata
