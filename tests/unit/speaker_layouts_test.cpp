#include <array>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string_view>

#include "adm/options.h"

#include "speaker_layouts.h"

namespace {

using mradm::render_layouts::SpeakerLayout;
using mradm::render_layouts::SpeakerSpec;

struct ExpectedOverride {
    std::string_view layout_id;
    std::string_view speaker_label;
    float azimuth;
    float elevation;
};

constexpr std::array<ExpectedOverride, 38> k_expected_overrides{{
    {"wav71", "M+135", 150.0F, 0.0F},   {"wav71", "M-135", -150.0F, 0.0F},   {"wav71", "M+090", 110.0F, 0.0F},
    {"wav71", "M-090", -110.0F, 0.0F},  {"2+5+0", "U+030", 90.0F, 45.0F},    {"2+5+0", "U-030", -90.0F, 45.0F},
    {"4+5+0", "U+030", 45.0F, 45.0F},   {"4+5+0", "U-030", -45.0F, 45.0F},   {"4+5+0", "U+110", 135.0F, 45.0F},
    {"4+5+0", "U-110", -135.0F, 45.0F}, {"4+7+0", "M+090", 110.0F, 0.0F},    {"4+7+0", "M-090", -110.0F, 0.0F},
    {"4+7+0", "M+135", 150.0F, 0.0F},   {"4+7+0", "M-135", -150.0F, 0.0F},   {"4+7+0", "U+045", 45.0F, 45.0F},
    {"4+7+0", "U-045", -45.0F, 45.0F},  {"4+7+0", "U+135", 135.0F, 45.0F},   {"4+7+0", "U-135", -135.0F, 45.0F},
    {"9.1.6", "M+070", 60.0F, 0.0F},    {"9.1.6", "M-070", -60.0F, 0.0F},    {"9.1.6", "U+070", 45.0F, 45.0F},
    {"9.1.6", "U-070", -45.0F, 45.0F},  {"9.1.6", "U+110", 90.0F, 45.0F},    {"9.1.6", "U-110", -90.0F, 45.0F},
    {"9.1.6", "U+150", 135.0F, 45.0F},  {"9.1.6", "U-150", -135.0F, 45.0F},  {"9+10+3", "M+135", 150.0F, 0.0F},
    {"9+10+3", "M-135", -150.0F, 0.0F}, {"9+10+3", "U+045", 45.0F, 45.0F},   {"9+10+3", "U-045", -45.0F, 45.0F},
    {"9+10+3", "U+135", 135.0F, 45.0F}, {"9+10+3", "U-135", -135.0F, 45.0F}, {"9+10+3", "U+090", 90.0F, 45.0F},
    {"9+10+3", "U-090", -90.0F, 45.0F}, {"9+10+3", "U+180", 180.0F, 45.0F},  {"9+10+3", "B+000", 0.0F, -15.0F},
    {"9+10+3", "B+045", 45.0F, -15.0F}, {"9+10+3", "B-045", -45.0F, -15.0F},
}};

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

const SpeakerSpec* find_speaker(const SpeakerLayout& layout, std::string_view label) {
    const auto it = std::ranges::find(layout.speakers, label, &SpeakerSpec::label);
    return it == layout.speakers.end() ? nullptr : &*it;
}

bool verify_profile_inventory() {
    using mradm::SpeakerGeometry;
    using mradm::render_layouts::find_speaker_layout;
    using mradm::render_layouts::speaker_layouts;

    const auto& standard = speaker_layouts(SpeakerGeometry::standard);
    const auto& apple = speaker_layouts(SpeakerGeometry::apple);
    bool ok = check(standard.size() == 9U, "standard profile keeps all nine built-in layouts");
    ok &= check(apple.size() == 7U, "Apple profile exposes the seven CoreAudio fixed layouts");
    ok &= check(&speaker_layouts() == &standard, "no-argument lookup remains the standard profile");
    ok &= check(find_speaker_layout("0+2+0", SpeakerGeometry::apple) == nullptr,
                "Apple profile does not invent a stereo CoreAudio profile");
    ok &= check(find_speaker_layout("4+5+4", SpeakerGeometry::apple) == nullptr,
                "Apple profile does not invent a 9.1.4 CoreAudio profile");

    constexpr std::array<std::string_view, 7> k_apple_ids{
        "0+5+0",
        "wav71",
        "2+5+0",
        "4+5+0",
        "4+7+0",
        "9.1.6",
        "9+10+3",
    };
    for (const auto id : k_apple_ids) {
        ok &= check(find_speaker_layout(id, SpeakerGeometry::apple) != nullptr, "Apple layout is present");
    }
    return ok;
}

bool verify_profile_structure_and_ranges() {
    using mradm::SpeakerGeometry;
    using mradm::render_layouts::find_speaker_layout;
    using mradm::render_layouts::speaker_layouts;

    bool ok = true;
    std::size_t changed_positions = 0U;
    for (const auto& apple_layout : speaker_layouts(SpeakerGeometry::apple)) {
        const auto* standard_layout = find_speaker_layout(apple_layout.id, SpeakerGeometry::standard);
        ok &= check(standard_layout != nullptr, "Apple layout has a standard-profile counterpart");
        if (standard_layout == nullptr) {
            continue;
        }
        ok &= check(apple_layout.display_name == standard_layout->display_name, "display name is profile-independent");
        ok &= check(apple_layout.speakers.size() == standard_layout->speakers.size(),
                    "channel count is profile-independent");
        for (std::size_t channel = 0U; channel < apple_layout.speakers.size(); ++channel) {
            const auto& apple_speaker = apple_layout.speakers[channel];
            const auto& standard_speaker = standard_layout->speakers[channel];
            ok &= check(apple_speaker.label == standard_speaker.label, "channel label/order is profile-independent");
            ok &= check(apple_speaker.is_lfe == standard_speaker.is_lfe, "LFE identity is profile-independent");
            if (apple_speaker.azimuth != standard_speaker.azimuth ||
                apple_speaker.elevation != standard_speaker.elevation) {
                ++changed_positions;
            }
            if (apple_speaker.is_lfe) {
                ok &= check(apple_speaker.azimuth == standard_speaker.azimuth &&
                                apple_speaker.elevation == standard_speaker.elevation,
                            "LFE placeholders are not part of the geometry switch");
            } else {
                ok &= check(apple_speaker.azimuth_range.has_value() && apple_speaker.elevation_range.has_value(),
                            "Apple non-LFE position has fixed ranges");
                if (apple_speaker.azimuth_range && apple_speaker.elevation_range) {
                    ok &= check(apple_speaker.azimuth_range->first == apple_speaker.azimuth &&
                                    apple_speaker.azimuth_range->second == apple_speaker.azimuth &&
                                    apple_speaker.elevation_range->first == apple_speaker.elevation &&
                                    apple_speaker.elevation_range->second == apple_speaker.elevation,
                                "Apple position ranges collapse to the effective coordinate");
                }
            }
        }
    }
    ok &= check(changed_positions == k_expected_overrides.size(), "only the 38 measured non-LFE positions change");
    return ok;
}

bool verify_expected_coordinates() {
    using mradm::SpeakerGeometry;
    using mradm::render_layouts::find_speaker_layout;

    bool ok = true;
    for (const auto& expected : k_expected_overrides) {
        const auto* layout = find_speaker_layout(expected.layout_id, SpeakerGeometry::apple);
        ok &= check(layout != nullptr, "expected Apple layout exists");
        if (layout == nullptr) {
            continue;
        }
        const auto* speaker = find_speaker(*layout, expected.speaker_label);
        ok &= check(speaker != nullptr, "expected Apple speaker exists");
        if (speaker != nullptr) {
            ok &= check(speaker->azimuth == expected.azimuth && speaker->elevation == expected.elevation,
                        "Apple speaker coordinate matches the embedded CoreAudio value");
            ok &= check(!speaker->is_lfe, "geometry override never targets LFE");
        }
    }

    const auto* apple_51 = find_speaker_layout("0+5+0", SpeakerGeometry::apple);
    const auto* standard_51 = find_speaker_layout("0+5+0", SpeakerGeometry::standard);
    ok &= check(apple_51 != nullptr && standard_51 != nullptr, "5.1 exists in both profiles");
    if (apple_51 != nullptr && standard_51 != nullptr) {
        for (std::size_t channel = 0U; channel < apple_51->speakers.size(); ++channel) {
            ok &= check(apple_51->speakers[channel].azimuth == standard_51->speakers[channel].azimuth &&
                            apple_51->speakers[channel].elevation == standard_51->speakers[channel].elevation,
                        "5.1 effective coordinates are identical between profiles");
        }
    }
    return ok;
}

} // namespace

int main() {
    const bool ok =
        verify_profile_inventory() && verify_profile_structure_and_ranges() && verify_expected_coordinates();
    if (ok) {
        std::cout << "speaker layouts test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
