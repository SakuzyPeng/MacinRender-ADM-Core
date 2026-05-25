#include "speaker_layouts.h"

#include <algorithm>

namespace mradm::render_layouts {

namespace {

using Range = std::pair<float, float>;
using OptRange = std::optional<Range>;

SpeakerSpec sp(std::string_view label,
               float azimuth,
               float elevation,
               bool is_lfe = false,
               OptRange azimuth_range = std::nullopt,
               OptRange elevation_range = std::nullopt) {
    return {azimuth, elevation, label, is_lfe, azimuth_range, elevation_range};
}

Range range(float min, float max) {
    return {min, max};
}

} // namespace

// NOLINTNEXTLINE(readability-function-size): static data table for supported loudspeaker layouts.
const std::vector<SpeakerLayout>& speaker_layouts() {
    // clang-format off
    static const std::vector<SpeakerLayout> layouts{
        {"0+2+0", "Stereo",
         {
             sp("M+030", 30.0F, 0.0F, false, range(30.0F, 30.0F), range(0.0F, 0.0F)),
             sp("M-030", -30.0F, 0.0F, false, range(-30.0F, -30.0F), range(0.0F, 0.0F)),
         }},
        {"0+5+0", "5.1",
         {
             sp("M+030", 30.0F, 0.0F, false, range(30.0F, 30.0F), range(0.0F, 0.0F)),
             sp("M-030", -30.0F, 0.0F, false, range(-30.0F, -30.0F), range(0.0F, 0.0F)),
             sp("M+000", 0.0F, 0.0F, false, range(0.0F, 0.0F), range(0.0F, 0.0F)),
             sp("LFE1", 45.0F, -30.0F, true, range(-180.0F, 180.0F), range(-90.0F, 90.0F)),
             sp("M+110", 110.0F, 0.0F, false, range(100.0F, 120.0F), range(0.0F, 15.0F)),
             sp("M-110", -110.0F, 0.0F, false, range(-120.0F, -100.0F), range(0.0F, 15.0F)),
         }},
        {"2+5+0", "5.1.2",
         {
             sp("M+030", 30.0F, 0.0F, false, range(30.0F, 30.0F), range(0.0F, 0.0F)),
             sp("M-030", -30.0F, 0.0F, false, range(-30.0F, -30.0F), range(0.0F, 0.0F)),
             sp("M+000", 0.0F, 0.0F, false, range(0.0F, 0.0F), range(0.0F, 0.0F)),
             sp("LFE1", 45.0F, -30.0F, true, range(-180.0F, 180.0F), range(-90.0F, 90.0F)),
             sp("M+110", 110.0F, 0.0F, false, range(100.0F, 120.0F), range(0.0F, 15.0F)),
             sp("M-110", -110.0F, 0.0F, false, range(-120.0F, -100.0F), range(0.0F, 15.0F)),
             sp("U+030", 30.0F, 30.0F, false, range(30.0F, 45.0F), range(30.0F, 55.0F)),
             sp("U-030", -30.0F, 30.0F, false, range(-45.0F, -30.0F), range(30.0F, 55.0F)),
         }},
        {"4+5+0", "5.1.4",
         {
             sp("M+030", 30.0F, 0.0F, false, range(30.0F, 30.0F), range(0.0F, 0.0F)),
             sp("M-030", -30.0F, 0.0F, false, range(-30.0F, -30.0F), range(0.0F, 0.0F)),
             sp("M+000", 0.0F, 0.0F, false, range(0.0F, 0.0F), range(0.0F, 0.0F)),
             sp("LFE1", 45.0F, -30.0F, true, range(-180.0F, 180.0F), range(-90.0F, 90.0F)),
             sp("M+110", 110.0F, 0.0F, false, range(100.0F, 120.0F), range(0.0F, 0.0F)),
             sp("M-110", -110.0F, 0.0F, false, range(-120.0F, -100.0F), range(0.0F, 0.0F)),
             sp("U+030", 30.0F, 30.0F, false, range(30.0F, 45.0F), range(30.0F, 55.0F)),
             sp("U-030", -30.0F, 30.0F, false, range(-45.0F, -30.0F), range(30.0F, 55.0F)),
             sp("U+110", 110.0F, 30.0F, false, range(100.0F, 135.0F), range(30.0F, 55.0F)),
             sp("U-110", -110.0F, 30.0F, false, range(-135.0F, -100.0F), range(30.0F, 55.0F)),
         }},
        {"wav71", "7.1",
         {
             sp("M+030", 30.0F, 0.0F),
             sp("M-030", -30.0F, 0.0F),
             sp("M+000", 0.0F, 0.0F),
             sp("LFE1", 45.0F, -30.0F, true),
             sp("M+135", 135.0F, 0.0F),
             sp("M-135", -135.0F, 0.0F),
             sp("M+090", 90.0F, 0.0F),
             sp("M-090", -90.0F, 0.0F),
         }},
        {"4+7+0", "7.1.4",
         {
             sp("M+030", 30.0F, 0.0F, false, range(30.0F, 45.0F), range(0.0F, 0.0F)),
             sp("M-030", -30.0F, 0.0F, false, range(-45.0F, -30.0F), range(0.0F, 0.0F)),
             sp("M+000", 0.0F, 0.0F, false, range(0.0F, 0.0F), range(0.0F, 0.0F)),
             sp("LFE1", 45.0F, -30.0F, true, range(-180.0F, 180.0F), range(-90.0F, 90.0F)),
             sp("M+090", 90.0F, 0.0F, false, range(85.0F, 110.0F), range(0.0F, 0.0F)),
             sp("M-090", -90.0F, 0.0F, false, range(-110.0F, -85.0F), range(0.0F, 0.0F)),
             sp("M+135", 135.0F, 0.0F, false, range(120.0F, 150.0F), range(0.0F, 0.0F)),
             sp("M-135", -135.0F, 0.0F, false, range(-150.0F, -120.0F), range(0.0F, 0.0F)),
             sp("U+045", 45.0F, 30.0F, false, range(30.0F, 45.0F), range(30.0F, 55.0F)),
             sp("U-045", -45.0F, 30.0F, false, range(-45.0F, -30.0F), range(30.0F, 55.0F)),
             sp("U+135", 135.0F, 30.0F, false, range(100.0F, 150.0F), range(30.0F, 55.0F)),
             sp("U-135", -135.0F, 30.0F, false, range(-150.0F, -100.0F), range(30.0F, 55.0F)),
         }},
        {"4+5+4", "9.1.4",
         {
             sp("M+030", 30.0F, 0.0F),
             sp("M-030", -30.0F, 0.0F),
             sp("M+000", 0.0F, 0.0F),
             sp("LFE1", 45.0F, -30.0F, true),
             sp("M+110", 110.0F, 0.0F),
             sp("M-110", -110.0F, 0.0F),
             sp("M+150", 150.0F, 0.0F),
             sp("M-150", -150.0F, 0.0F),
             sp("M+070", 70.0F, 0.0F),
             sp("M-070", -70.0F, 0.0F),
             sp("U+070", 70.0F, 45.0F),
             sp("U-070", -70.0F, 45.0F),
             sp("U+150", 150.0F, 45.0F),
             sp("U-150", -150.0F, 45.0F),
         }},
        {"9.1.6", "9.1.6 (Dolby Atmos)",
         {
             sp("M+030", 30.0F, 0.0F),
             sp("M-030", -30.0F, 0.0F),
             sp("M+000", 0.0F, 0.0F),
             sp("LFE1", 45.0F, -30.0F, true),
             sp("M+110", 110.0F, 0.0F),
             sp("M-110", -110.0F, 0.0F),
             sp("M+150", 150.0F, 0.0F),
             sp("M-150", -150.0F, 0.0F),
             sp("M+070", 70.0F, 0.0F),
             sp("M-070", -70.0F, 0.0F),
             sp("U+070", 70.0F, 45.0F),
             sp("U-070", -70.0F, 45.0F),
             sp("U+110", 110.0F, 45.0F),
             sp("U-110", -110.0F, 45.0F),
             sp("U+150", 150.0F, 45.0F),
             sp("U-150", -150.0F, 45.0F),
         }},
        {"9+10+3", "22.2",
         {
             sp("M+060", 60.0F, 0.0F),
             sp("M-060", -60.0F, 0.0F),
             sp("M+000", 0.0F, 0.0F),
             sp("LFE1", 45.0F, -30.0F, true),
             sp("M+135", 135.0F, 0.0F),
             sp("M-135", -135.0F, 0.0F),
             sp("M+030", 30.0F, 0.0F),
             sp("M-030", -30.0F, 0.0F),
             sp("M+180", 180.0F, 0.0F),
             sp("LFE2", -45.0F, -30.0F, true),
             sp("M+090", 90.0F, 0.0F),
             sp("M-090", -90.0F, 0.0F),
             sp("U+045", 45.0F, 30.0F),
             sp("U-045", -45.0F, 30.0F),
             sp("U+000", 0.0F, 30.0F),
             sp("T+000", 0.0F, 90.0F),
             sp("U+135", 135.0F, 30.0F),
             sp("U-135", -135.0F, 30.0F),
             sp("U+090", 90.0F, 30.0F),
             sp("U-090", -90.0F, 30.0F),
             sp("U+180", 180.0F, 30.0F),
             sp("B+000", 0.0F, -30.0F),
             sp("B+045", 45.0F, -30.0F),
             sp("B-045", -45.0F, -30.0F),
         }},
    };
    // clang-format on
    return layouts;
}

const SpeakerLayout* find_speaker_layout(std::string_view id) {
    const auto& layouts = speaker_layouts();
    const auto it = std::ranges::find_if(layouts, [id](const SpeakerLayout& layout) { return layout.id == id; });
    if (it == layouts.end()) {
        return nullptr;
    }
    return &*it;
}

} // namespace mradm::render_layouts
