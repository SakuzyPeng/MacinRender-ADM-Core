#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <spatialaudioclient.h>
#include <string_view>

// Windows-only speaker-layout table for the system-spatial monitor sink. The monitor renders a
// multichannel bed (EAR / VBAP / Apple) and this sink hands it to ISpatialAudioClient; the active
// Windows spatializer (Windows Sonic for Headphones / Dolby Atmos for Headphones / DTS Headphone:X)
// then HRTF-renders it to the headphone route. This is the Windows analog of adm_apple/apple_layouts.h
// + the ASBR sink.
//
// Each bed channel is routed one of two ways (see ChannelRoute):
//   - STATIC bed slot: the channel maps to one of the spatializer's 17 named static positions
//     (the 8.1.4.4 set — front/side/back ring + LFE + 4 top + 4 bottom). Its geometry is the
//     spatializer's own (unpublished) angle; we only assert the *name*.
//   - DYNAMIC object: the channel is a speaker position the static enum has no name for (e.g. the
//     9.1.6 wide pair Lw/Rw and top-middle pair Ltm/Rtm, and the 22.2 positions with no static
//     Windows name). We activate it as AudioObjectType_Dynamic and pin it with SetPosition(x,y,z)
//     at the speaker's exact angle, so its geometry is *ours*.
//
// The crux is the per-channel mapping. The bed the sink receives is in the project's canonical
// channel order — the same CoreAudio Atmos order the CAF / APAC writers use (see
// adm_engine/layout_table.cpp) and the same speaker geometry the renderers use (see
// adm_render_common/speaker_layouts.cpp), because the monitor renders with output_layout exactly as
// the offline CAF path does:
//   7.1.4 (4+7+0): L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr
//   5.1.4 (4+5+0): L R C LFE Ls Rs Vhl Vhr Ltr Rtr
//   5.1   (0+5+0): L R C LFE Ls Rs
//   9.1.6         : L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr
//   22.2 (9+10+3): M+060 M-060 M+000 LFE1 M+135 M-135 M+030 M-030 M+180 LFE2 M+090 M-090
//                  U+045 U-045 U+000 T+000 U+135 U-135 U+090 U-090 U+180 B+000 B+045 B-045
// Each entry's routes[] lists the ChannelRoute for that channel index, *in this order*, so channel i
// of the interleaved pull() buffer drives routes[i]. A wrong mapping swaps speaker positions
// (front-left leaks elsewhere), so the table is verified by ear on real hardware with a per-channel
// test tone. Windows COM / SpatialAudio types stay confined to adm_windows (ADR 0003); the device
// factory returns the third-party-free IAudioOutputDevice.
namespace mradm::windows_layouts {

inline constexpr uint16_t k_auto_object_slot = std::numeric_limits<uint16_t>::max();

// One bed channel's routing: either a named static position, or a dynamic object pinned in space.
// For static routes `type` is the named AudioObjectType and x/y/z are unused; for dynamic routes
// `type` is AudioObjectType_Dynamic and (x,y,z) is the SetPosition coordinate (right-handed, meters,
// origin at the listener's ear-center, +X right / +Y up / +Z behind). By default each input channel
// gets its own output object slot; `object_slot` is only set when multiple input channels intentionally
// feed the same spatial object (22.2 LFE1/LFE2 fold-down).
struct ChannelRoute {
    AudioObjectType type{AudioObjectType_None};
    bool is_dynamic{false};
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
    uint16_t object_slot{k_auto_object_slot};
};

// A named static bed slot.
[[nodiscard]] inline constexpr ChannelRoute bed(AudioObjectType type, uint16_t object_slot = k_auto_object_slot) {
    return {type, false, 0.0F, 0.0F, 0.0F, object_slot};
}

// A dynamic object pinned at a fixed position. Compute (x,y,z) from the speaker's BS.2051 azimuth A
// (project convention: 0=front, +ve=left) and elevation E (+ve=up) at unit radius 1 m:
//   x = -cos(E)*sin(A),  y = sin(E),  z = -cos(E)*cos(A)
[[nodiscard]] inline constexpr ChannelRoute obj(float x, float y, float z, uint16_t object_slot = k_auto_object_slot) {
    return {AudioObjectType_Dynamic, true, x, y, z, object_slot};
}

// In 7.1.4 the project's "Ls/Rs" are the *side* surrounds (the rears are the separate "Rls/Rrs"
// pair), so they map to SideLeft/SideRight and the rears to BackLeft/BackRight. The four height
// channels split front (Vhl/Vhr → TopFront*) and rear (Ltr/Rtr → TopBack*). All static.
inline constexpr std::array<ChannelRoute, 12> k_routes_7_1_4{{
    bed(AudioObjectType_FrontLeft),
    bed(AudioObjectType_FrontRight),
    bed(AudioObjectType_FrontCenter),
    bed(AudioObjectType_LowFrequency),
    bed(AudioObjectType_SideLeft),
    bed(AudioObjectType_SideRight),
    bed(AudioObjectType_BackLeft),
    bed(AudioObjectType_BackRight),
    bed(AudioObjectType_TopFrontLeft),
    bed(AudioObjectType_TopFrontRight),
    bed(AudioObjectType_TopBackLeft),
    bed(AudioObjectType_TopBackRight),
}};

// 5.1.4 has no separate rear pair, so the single surround pair (Ls/Rs) maps to the back positions —
// matching the project's 5.1 convention (WAVEFORMATEXTENSIBLE 5.1 mask uses BACK_LEFT/BACK_RIGHT) —
// plus the four Atmos height channels. All static.
inline constexpr std::array<ChannelRoute, 10> k_routes_5_1_4{{
    bed(AudioObjectType_FrontLeft),
    bed(AudioObjectType_FrontRight),
    bed(AudioObjectType_FrontCenter),
    bed(AudioObjectType_LowFrequency),
    bed(AudioObjectType_BackLeft),
    bed(AudioObjectType_BackRight),
    bed(AudioObjectType_TopFrontLeft),
    bed(AudioObjectType_TopFrontRight),
    bed(AudioObjectType_TopBackLeft),
    bed(AudioObjectType_TopBackRight),
}};

// 5.1: front L/R/C, LFE, and the surround pair as back positions (5.1 mask convention). All static.
inline constexpr std::array<ChannelRoute, 6> k_routes_5_1{{
    bed(AudioObjectType_FrontLeft),
    bed(AudioObjectType_FrontRight),
    bed(AudioObjectType_FrontCenter),
    bed(AudioObjectType_LowFrequency),
    bed(AudioObjectType_BackLeft),
    bed(AudioObjectType_BackRight),
}};

// 9.1.6 (Dolby Atmos) breaks the 7.1.4 static ceiling: 12 channels land on named static slots, and
// the 4 positions the 8.1.4.4 enum has no name for become dynamic objects pinned at their exact
// BS.2051 angles (from speaker_layouts.cpp's "9.1.6" entry, so the objects share the renderer's
// geometry). The dynamic positions are computed from azimuth/elevation via obj()'s formula:
//   Lw  M+070 (A= 70, E= 0):  (-cos0*sin70,  sin0,  -cos0*cos70)  = (-0.93969, 0.0,     -0.34202)
//   Rw  M-070 (A=-70, E= 0):  (+0.93969, 0.0, -0.34202)
//   Ltm U+110 (A= 110, E=45): (-cos45*sin110, sin45, -cos45*cos110) = (-0.66446, 0.70711, 0.24185)
//   Rtm U-110 (A=-110, E=45): (+0.66446, 0.70711, 0.24185)
// The static side (Ls/Rs at ±110°, Vhl/Vhr at azimuth ±70°) is rendered at those angles but replayed
// at the spatializer's own static positions — a geometry seam to verify by ear (esp. around the wide
// speakers, which sit between the static L and Ls).
inline constexpr std::array<ChannelRoute, 16> k_routes_9_1_6{{
    bed(AudioObjectType_FrontLeft),     // L   M+030
    bed(AudioObjectType_FrontRight),    // R   M-030
    bed(AudioObjectType_FrontCenter),   // C   M+000
    bed(AudioObjectType_LowFrequency),  // LFE
    bed(AudioObjectType_SideLeft),      // Ls  M+110
    bed(AudioObjectType_SideRight),     // Rs  M-110
    bed(AudioObjectType_BackLeft),      // Rls M+150
    bed(AudioObjectType_BackRight),     // Rrs M-150
    obj(-0.93969F, 0.0F, -0.34202F),    // Lw  M+070  (dynamic: no static "wide" name)
    obj(0.93969F, 0.0F, -0.34202F),     // Rw  M-070
    bed(AudioObjectType_TopFrontLeft),  // Vhl U+070
    bed(AudioObjectType_TopFrontRight), // Vhr U-070
    obj(-0.66446F, 0.70711F, 0.24185F), // Ltm U+110  (dynamic: no static "top-side" name)
    obj(0.66446F, 0.70711F, 0.24185F),  // Rtm U-110
    bed(AudioObjectType_TopBackLeft),   // Ltr U+150
    bed(AudioObjectType_TopBackRight),  // Rtr U-150
}};

// 22.2 uses the project/libear BS.2051 order. Windows exposes one non-spatialized static LFE slot,
// so LFE1 and LFE2 intentionally share object slot 3; the device mixes them into that one
// AudioObjectType_LowFrequency object instead of creating a directional dynamic object for LFE2.
// The remaining positions use static names where Windows has a clear slot and dynamic objects where
// it does not (front-wide, top-center/top-side/top-back-center, and bottom-center).
inline constexpr std::array<ChannelRoute, 24> k_routes_22_2{{
    obj(-0.86603F, 0.0F, -0.50000F, 0),        // M+060 (dynamic: no static "wide" name)
    obj(0.86603F, 0.0F, -0.50000F, 1),         // M-060
    bed(AudioObjectType_FrontCenter, 2),       // M+000
    bed(AudioObjectType_LowFrequency, 3),      // LFE1
    bed(AudioObjectType_BackLeft, 4),          // M+135
    bed(AudioObjectType_BackRight, 5),         // M-135
    bed(AudioObjectType_FrontLeft, 6),         // M+030
    bed(AudioObjectType_FrontRight, 7),        // M-030
    bed(AudioObjectType_BackCenter, 8),        // M+180
    bed(AudioObjectType_LowFrequency, 3),      // LFE2 (fold into the one Windows LFE slot)
    bed(AudioObjectType_SideLeft, 9),          // M+090
    bed(AudioObjectType_SideRight, 10),        // M-090
    bed(AudioObjectType_TopFrontLeft, 11),     // U+045
    bed(AudioObjectType_TopFrontRight, 12),    // U-045
    obj(0.0F, 0.50000F, -0.86603F, 13),        // U+000
    obj(0.0F, 1.00000F, 0.0F, 14),             // T+000
    bed(AudioObjectType_TopBackLeft, 15),      // U+135
    bed(AudioObjectType_TopBackRight, 16),     // U-135
    obj(-0.86603F, 0.50000F, 0.0F, 17),        // U+090
    obj(0.86603F, 0.50000F, 0.0F, 18),         // U-090
    obj(0.0F, 0.50000F, 0.86603F, 19),         // U+180
    obj(0.0F, -0.50000F, -0.86603F, 20),       // B+000
    bed(AudioObjectType_BottomFrontLeft, 21),  // B+045
    bed(AudioObjectType_BottomFrontRight, 22), // B-045
}};

struct WindowsSpeakerLayout {
    std::string_view id;
    std::string_view display_name;
    uint16_t channels{0};
    std::span<const ChannelRoute> routes;
};

// 22.2 is deliberately not advertised here. The native 22.2 route table above needs 23 distinct
// Windows spatial objects (24 input channels minus the shared LFE object), and the consumer
// spatializers tested so far crash inside CompPkgSup.dll when playback starts. Folding 22.2 into a
// smaller object budget would no longer be native 22.2, so callers should see "unsupported" instead
// of getting an approximated bed.
// clang-format off
inline constexpr std::array<WindowsSpeakerLayout, 4> k_windows_speaker_layouts{{
    {"4+7+0", "7.1.4", 12, k_routes_7_1_4},
    {"4+5+0", "5.1.4", 10, k_routes_5_1_4},
    {"0+5+0", "5.1",   6,  k_routes_5_1},
    {"9.1.6", "9.1.6", 16, k_routes_9_1_6},
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

[[nodiscard]] inline std::size_t object_slot(const ChannelRoute& route, std::size_t channel_index) {
    if (route.object_slot != k_auto_object_slot) {
        return route.object_slot;
    }
    return channel_index;
}

// Number of distinct spatial objects the layout activates. Usually this equals the channel count;
// 22.2 is one smaller because its two LFE input channels share the single Windows LFE object.
[[nodiscard]] inline std::size_t object_count(const WindowsSpeakerLayout& layout) {
    std::size_t count = 0;
    for (std::size_t ch = 0; ch < layout.routes.size(); ++ch) {
        count = std::max(count, object_slot(layout.routes[ch], ch) + 1U);
    }
    return count;
}

// OR of every *static* route's object type — the StaticObjectTypeMask passed to
// SpatialAudioObjectRenderStreamActivationParams. Dynamic objects are not part of the mask.
[[nodiscard]] inline AudioObjectType static_object_mask(const WindowsSpeakerLayout& layout) {
    auto mask = static_cast<uint32_t>(AudioObjectType_None);
    for (const ChannelRoute& route : layout.routes) {
        if (!route.is_dynamic) {
            mask |= static_cast<uint32_t>(route.type);
        }
    }
    return static_cast<AudioObjectType>(mask);
}

// Number of dynamic objects the layout needs — the Min/MaxDynamicObjectCount requested at stream
// activation (0 for the all-static layouts, so their activation params are unchanged).
[[nodiscard]] inline uint32_t dynamic_object_count(const WindowsSpeakerLayout& layout) {
    return static_cast<uint32_t>(
        std::ranges::count_if(layout.routes, [](const ChannelRoute& r) { return r.is_dynamic; }));
}

} // namespace mradm::windows_layouts
