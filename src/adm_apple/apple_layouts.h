#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include <AudioToolbox/AudioToolbox.h>

// Shared CoreAudio speaker-layout table for the Apple backend. Both the AUSpatialMixer
// renderer (spatial_mixer_renderer.cpp) and the AVSampleBufferAudioRenderer monitor sink
// (avsamplebuffer_device.mm) resolve a project layout id to the canonical
// AudioChannelLayoutTag here, so the AU's output order and the system-spatialization sink
// share one source of truth. Apple framework types stay confined to adm_apple (ADR 0003).
namespace mradm::apple_layouts {

// CoreAudio output channel-layout tags, matching caf_io.cpp's canonical project-layout ->
// AudioChannelLayoutTag mapping so channel order is identical to the container writers'
// expected order (verified: VBAP pans ADM-left -> output channel 0).
inline constexpr AudioChannelLayoutTag k_tag_mpeg_5_1_a = (121U << 16) | 6U;
inline constexpr AudioChannelLayoutTag k_tag_wave_7_1 = (189U << 16) | 8U;
inline constexpr AudioChannelLayoutTag k_tag_atmos_5_1_2 = (194U << 16) | 8U;
inline constexpr AudioChannelLayoutTag k_tag_atmos_5_1_4 = (195U << 16) | 10U;
inline constexpr AudioChannelLayoutTag k_tag_atmos_7_1_4 = (192U << 16) | 12U;
inline constexpr AudioChannelLayoutTag k_tag_atmos_9_1_6 = (193U << 16) | 16U;
inline constexpr AudioChannelLayoutTag k_tag_cicp_13 = (204U << 16) | 24U;

struct AppleSpeakerLayout {
    std::string_view id;
    std::string_view display_name;
    uint16_t channels;
    uint16_t lfe_count;
    bool is_3d;
    AudioChannelLayoutTag layout_tag;
};

// clang-format off
inline constexpr std::array<AppleSpeakerLayout, 7> k_apple_speaker_layouts{{
    {"0+5+0",  "5.1",   6,  1, false, k_tag_mpeg_5_1_a},
    {"wav71",  "7.1",   8,  1, false, k_tag_wave_7_1},
    {"2+5+0",  "5.1.2", 8,  1, true,  k_tag_atmos_5_1_2},
    {"4+5+0",  "5.1.4", 10, 1, true,  k_tag_atmos_5_1_4},
    {"4+7+0",  "7.1.4", 12, 1, true,  k_tag_atmos_7_1_4},
    {"9.1.6",  "9.1.6", 16, 1, true,  k_tag_atmos_9_1_6},
    {"9+10+3", "22.2",  24, 2, true,  k_tag_cicp_13},
}};
// clang-format on

[[nodiscard]] inline const AppleSpeakerLayout* find_apple_speaker_layout(std::string_view layout_id) {
    const auto* const it = std::ranges::find_if(
        k_apple_speaker_layouts, [layout_id](const AppleSpeakerLayout& layout) { return layout.id == layout_id; });
    if (it == k_apple_speaker_layouts.end()) {
        return nullptr;
    }
    return std::addressof(*it);
}

} // namespace mradm::apple_layouts
