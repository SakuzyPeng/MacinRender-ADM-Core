#include "speaker_layouts.h"

#include <algorithm>
#include <array>
#include <cassert>

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

namespace {

// NOLINTNEXTLINE(readability-function-size): static data table for supported loudspeaker layouts.
const std::vector<SpeakerLayout>& standard_speaker_layouts() {
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

struct SpeakerPositionOverride {
    std::string_view layout_id;
    std::string_view speaker_label;
    float azimuth{0.0F};
    float elevation{0.0F};
};

// Captured on macOS 27.0 (26A5406e) with AudioFormatGetProperty using
// kAudioFormatProperty_ChannelLayoutForTag. CoreAudio expands these standard
// layout tags to fixed spherical coordinates:
// MPEG_5_1_A, WAVE_7_1, Atmos_5_1_2, Atmos_5_1_4, Atmos_7_1_4,
// Atmos_9_1_6 and CICP_13. Coordinates below use the project's ADM convention
// (+azimuth = left), the inverse of CoreAudio's +right convention. LFE coordinates
// are deliberately left as project placeholders because LFE never participates in
// object panning.
// clang-format off
constexpr std::array<std::string_view, 7> k_apple_layout_ids{
    "0+5+0", "wav71", "2+5+0", "4+5+0", "4+7+0", "9.1.6", "9+10+3",
};

constexpr std::array<SpeakerPositionOverride, 38> k_apple_position_overrides{{
    {"wav71",  "M+135",  150.0F,   0.0F}, {"wav71",  "M-135", -150.0F,   0.0F},
    {"wav71",  "M+090",  110.0F,   0.0F}, {"wav71",  "M-090", -110.0F,   0.0F},

    {"2+5+0",  "U+030",   90.0F,  45.0F}, {"2+5+0",  "U-030",  -90.0F,  45.0F},

    {"4+5+0",  "U+030",   45.0F,  45.0F}, {"4+5+0",  "U-030",  -45.0F,  45.0F},
    {"4+5+0",  "U+110",  135.0F,  45.0F}, {"4+5+0",  "U-110", -135.0F,  45.0F},

    {"4+7+0",  "M+090",  110.0F,   0.0F}, {"4+7+0",  "M-090", -110.0F,   0.0F},
    {"4+7+0",  "M+135",  150.0F,   0.0F}, {"4+7+0",  "M-135", -150.0F,   0.0F},
    {"4+7+0",  "U+045",   45.0F,  45.0F}, {"4+7+0",  "U-045",  -45.0F,  45.0F},
    {"4+7+0",  "U+135",  135.0F,  45.0F}, {"4+7+0",  "U-135", -135.0F,  45.0F},

    {"9.1.6",  "M+070",   60.0F,   0.0F}, {"9.1.6",  "M-070",  -60.0F,   0.0F},
    {"9.1.6",  "U+070",   45.0F,  45.0F}, {"9.1.6",  "U-070",  -45.0F,  45.0F},
    {"9.1.6",  "U+110",   90.0F,  45.0F}, {"9.1.6",  "U-110",  -90.0F,  45.0F},
    {"9.1.6",  "U+150",  135.0F,  45.0F}, {"9.1.6",  "U-150", -135.0F,  45.0F},

    {"9+10+3", "M+135",  150.0F,   0.0F}, {"9+10+3", "M-135", -150.0F,   0.0F},
    {"9+10+3", "U+045",   45.0F,  45.0F}, {"9+10+3", "U-045",  -45.0F,  45.0F},
    {"9+10+3", "U+135",  135.0F,  45.0F}, {"9+10+3", "U-135", -135.0F,  45.0F},
    {"9+10+3", "U+090",   90.0F,  45.0F}, {"9+10+3", "U-090",  -90.0F,  45.0F},
    {"9+10+3", "U+180",  180.0F,  45.0F},
    {"9+10+3", "B+000",    0.0F, -15.0F}, {"9+10+3", "B+045",   45.0F, -15.0F},
    {"9+10+3", "B-045",  -45.0F, -15.0F},
}};
// clang-format on

const std::vector<SpeakerLayout>& apple_speaker_layouts() {
    static const std::vector<SpeakerLayout> layouts = [] {
        std::vector<SpeakerLayout> result;
        result.reserve(k_apple_layout_ids.size());
        const auto& standard = standard_speaker_layouts();
        for (const auto layout_id : k_apple_layout_ids) {
            const auto it = std::ranges::find_if(
                standard, [layout_id](const SpeakerLayout& layout) { return layout.id == layout_id; });
            assert(it != standard.end());
            if (it == standard.end()) {
                continue;
            }
            result.push_back(*it);
        }

        for (const auto& position_override : k_apple_position_overrides) {
            const auto layout_it = std::ranges::find_if(result, [&position_override](const SpeakerLayout& layout) {
                return layout.id == position_override.layout_id;
            });
            assert(layout_it != result.end());
            if (layout_it == result.end()) {
                continue;
            }
            const auto speaker_it =
                std::ranges::find_if(layout_it->speakers, [&position_override](const SpeakerSpec& speaker) {
                    return speaker.label == position_override.speaker_label;
                });
            assert(speaker_it != layout_it->speakers.end());
            if (speaker_it == layout_it->speakers.end()) {
                continue;
            }
            speaker_it->azimuth = position_override.azimuth;
            speaker_it->elevation = position_override.elevation;
        }

        // This profile describes CoreAudio's fixed effective positions, not the
        // installation ranges carried by the ADM nominal layout.
        for (auto& layout : result) {
            for (auto& speaker : layout.speakers) {
                if (!speaker.is_lfe) {
                    speaker.azimuth_range = Range{speaker.azimuth, speaker.azimuth};
                    speaker.elevation_range = Range{speaker.elevation, speaker.elevation};
                }
            }
        }
        return result;
    }();
    return layouts;
}

} // namespace

const std::vector<SpeakerLayout>& speaker_layouts() {
    return standard_speaker_layouts();
}

const std::vector<SpeakerLayout>& speaker_layouts(SpeakerGeometry geometry) {
    return geometry == SpeakerGeometry::apple ? apple_speaker_layouts() : standard_speaker_layouts();
}

const SpeakerLayout* find_speaker_layout(std::string_view id) {
    return find_speaker_layout(id, SpeakerGeometry::standard);
}

const SpeakerLayout* find_speaker_layout(std::string_view id, SpeakerGeometry geometry) {
    const auto& layouts = speaker_layouts(geometry);
    const auto it = std::ranges::find_if(layouts, [id](const SpeakerLayout& layout) { return layout.id == id; });
    if (it == layouts.end()) {
        return nullptr;
    }
    return &*it;
}

} // namespace mradm::render_layouts
