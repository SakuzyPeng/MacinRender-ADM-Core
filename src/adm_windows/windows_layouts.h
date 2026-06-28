#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <spatialaudioclient.h>
#include <string_view>

// Windows-only speaker-layout table for the system-spatial monitor sink. The monitor renders a
// multichannel bed (EAR / VBAP / Apple) and this sink hands it to ISpatialAudioClient as a set of
// *static* spatial-audio objects, one per bed channel; the active Windows spatializer (Windows
// Sonic for Headphones / Dolby Atmos for Headphones / DTS Headphone:X) then HRTF-renders the bed
// to the headphone route. This is the Windows analog of adm_apple/apple_layouts.h + the ASBR sink.
//
// The crux is the per-channel AudioObjectType mapping. The bed the sink receives is in the project's
// canonical channel order — the same CoreAudio Atmos order the CAF / APAC writers use (see
// adm_engine/layout_table.cpp), because the monitor renders with output_layout exactly as the
// offline CAF path does:
//   7.1.4 (4+7+0): L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr
//   5.1.4 (4+5+0): L R C LFE Ls Rs Vhl Vhr Ltr Rtr
//   5.1   (0+5+0): L R C LFE Ls Rs
// Each entry's object_types[] lists the AudioObjectType for that channel index, *in this order*, so
// channel i of the interleaved pull() buffer drives object_types[i]. A wrong mapping swaps speaker
// positions (front-left leaks elsewhere), so the table is verified by ear on real hardware with a
// per-channel test tone. Windows COM / SpatialAudio types stay confined to adm_windows (ADR 0003);
// the device factory returns the third-party-free IAudioOutputDevice.
namespace mradm::windows_layouts {

// In 7.1.4 the project's "Ls/Rs" are the *side* surrounds (the rears are the separate "Rls/Rrs"
// pair), so they map to SideLeft/SideRight and the rears to BackLeft/BackRight. The four height
// channels split front (Vhl/Vhr → TopFront*) and rear (Ltr/Rtr → TopBack*).
inline constexpr std::array<AudioObjectType, 12> k_objects_7_1_4{
    AudioObjectType_FrontLeft,
    AudioObjectType_FrontRight,
    AudioObjectType_FrontCenter,
    AudioObjectType_LowFrequency,
    AudioObjectType_SideLeft,
    AudioObjectType_SideRight,
    AudioObjectType_BackLeft,
    AudioObjectType_BackRight,
    AudioObjectType_TopFrontLeft,
    AudioObjectType_TopFrontRight,
    AudioObjectType_TopBackLeft,
    AudioObjectType_TopBackRight,
};

// 5.1.4 has no separate rear pair, so the single surround pair (Ls/Rs) maps to the back positions —
// matching the project's 5.1 convention (WAVEFORMATEXTENSIBLE 5.1 mask uses BACK_LEFT/BACK_RIGHT) —
// plus the four Atmos height channels.
inline constexpr std::array<AudioObjectType, 10> k_objects_5_1_4{
    AudioObjectType_FrontLeft,
    AudioObjectType_FrontRight,
    AudioObjectType_FrontCenter,
    AudioObjectType_LowFrequency,
    AudioObjectType_BackLeft,
    AudioObjectType_BackRight,
    AudioObjectType_TopFrontLeft,
    AudioObjectType_TopFrontRight,
    AudioObjectType_TopBackLeft,
    AudioObjectType_TopBackRight,
};

// 5.1: front L/R/C, LFE, and the surround pair as back positions (5.1 mask convention).
inline constexpr std::array<AudioObjectType, 6> k_objects_5_1{
    AudioObjectType_FrontLeft,
    AudioObjectType_FrontRight,
    AudioObjectType_FrontCenter,
    AudioObjectType_LowFrequency,
    AudioObjectType_BackLeft,
    AudioObjectType_BackRight,
};

struct WindowsSpeakerLayout {
    std::string_view id;
    std::string_view display_name;
    uint16_t channels{0};
    std::span<const AudioObjectType> object_types;
};

// clang-format off
inline constexpr std::array<WindowsSpeakerLayout, 3> k_windows_speaker_layouts{{
    {"4+7+0", "7.1.4", 12, k_objects_7_1_4},
    {"4+5+0", "5.1.4", 10, k_objects_5_1_4},
    {"0+5+0", "5.1",   6,  k_objects_5_1},
}};
// clang-format on

[[nodiscard]] inline const WindowsSpeakerLayout* find_windows_speaker_layout(std::string_view layout_id) {
    const auto it = std::ranges::find_if(
        k_windows_speaker_layouts, [layout_id](const WindowsSpeakerLayout& layout) { return layout.id == layout_id; });
    if (it == k_windows_speaker_layouts.end()) {
        return nullptr;
    }
    return std::addressof(*it);
}

// OR of every object type in a layout — the StaticObjectTypeMask passed to
// SpatialAudioObjectRenderStreamActivationParams.
[[nodiscard]] inline AudioObjectType static_object_mask(const WindowsSpeakerLayout& layout) {
    auto mask = static_cast<uint32_t>(AudioObjectType_None);
    for (const AudioObjectType type : layout.object_types) {
        mask |= static_cast<uint32_t>(type);
    }
    return static_cast<AudioObjectType>(mask);
}

} // namespace mradm::windows_layouts
