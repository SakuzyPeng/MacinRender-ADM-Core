#include "speaker_layouts.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "layout_ffi.h"

namespace mradm::render_layouts {

namespace {

std::string_view text(const uint8_t* data, size_t len) {
    return {reinterpret_cast<const char*>(data), len};
}

std::optional<std::pair<float, float>> range(uint32_t present, float min, float max) {
    return present != 0 ? std::optional{std::pair{min, max}} : std::nullopt;
}

// The table data lives in Rust (mradm-scene `layouts`, PCM consistency 4.3); these views point at
// its process-lifetime strings.
std::vector<SpeakerLayout> load(SpeakerGeometry geometry) {
    const MradmLayoutEntry* entries = nullptr;
    size_t count = 0;
    std::array<uint8_t, 256> message{};
    const auto id = geometry == SpeakerGeometry::apple ? 1U : 0U;
    if (mradm_scene_layouts(id, &entries, &count, message.data(), message.size()) != 0) {
        throw std::runtime_error(std::string{"speaker layout table: "} + reinterpret_cast<const char*>(message.data()));
    }
    std::vector<SpeakerLayout> layouts;
    layouts.reserve(count);
    for (const auto& entry : std::span{entries, count}) {
        SpeakerLayout layout{text(entry.id, entry.id_len), text(entry.display_name, entry.display_name_len), {}};
        layout.speakers.reserve(entry.speakers_len);
        for (const auto& speaker : std::span{entry.speakers, entry.speakers_len}) {
            layout.speakers.push_back(
                {speaker.azimuth,
                 speaker.elevation,
                 text(speaker.label, speaker.label_len),
                 speaker.is_lfe != 0,
                 range(speaker.has_azimuth_range, speaker.azimuth_range[0], speaker.azimuth_range[1]),
                 range(speaker.has_elevation_range, speaker.elevation_range[0], speaker.elevation_range[1])});
        }
        layouts.push_back(std::move(layout));
    }
    return layouts;
}

} // namespace

const std::vector<SpeakerLayout>& speaker_layouts() {
    return speaker_layouts(SpeakerGeometry::standard);
}

const std::vector<SpeakerLayout>& speaker_layouts(SpeakerGeometry geometry) {
    static const std::vector<SpeakerLayout> standard = load(SpeakerGeometry::standard);
    static const std::vector<SpeakerLayout> apple = load(SpeakerGeometry::apple);
    return geometry == SpeakerGeometry::apple ? apple : standard;
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
