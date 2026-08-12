#pragma once

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/options.h"

namespace mradm::render_layouts {

struct SpeakerSpec {
    float azimuth{0.0F};
    float elevation{0.0F};
    std::string_view label;
    bool is_lfe{false};
    std::optional<std::pair<float, float>> azimuth_range;
    std::optional<std::pair<float, float>> elevation_range;
};

struct SpeakerLayout {
    std::string_view id;
    std::string_view display_name;
    std::vector<SpeakerSpec> speakers;
};

const std::vector<SpeakerLayout>& speaker_layouts();
const std::vector<SpeakerLayout>& speaker_layouts(SpeakerGeometry geometry);
const SpeakerLayout* find_speaker_layout(std::string_view id);
const SpeakerLayout* find_speaker_layout(std::string_view id, SpeakerGeometry geometry);

} // namespace mradm::render_layouts
