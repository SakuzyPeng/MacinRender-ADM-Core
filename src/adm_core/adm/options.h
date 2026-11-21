#pragma once

// The render settings the Apple island reads (RenderPlan in render.h); the engine's options are in Rust.
// The standard headers stay as before for the islands that rely on them transitively.
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"

namespace mradm {

enum class SpeakerSpreadMode {
    automatic, // Triple Balance size; otherwise MDAP for 3D layouts, none for 2D
    none,      // ignore extent and use the selected renderer's point panner
    mdap,      // always use MDAP (multi-directional amplitude panning) for 3D layouts
};

// Routing policy for ADM DirectSpeakers content. `automatic` is resolved by the
// selected backend: SAF and Apple loudspeaker output use label routing, while
// Apple binaural output uses position routing. `matrix` uses a user-supplied
// sparse label-to-label gain matrix on EAR / SAF / Apple speaker outputs.
enum class DirectSpeakersRoutingMode {
    automatic,
    label,
    position,
    matrix,
};

enum class BinauralSpreadMode {
    automatic,    // cloud
    none,         // point source; extent parameters ignored
    cloud,        // 17-point angular extent cloud (default)
    saf_spreader, // experimental: SAF spreader OM mode (covariance-matching STFT domain)
};

// Routing for semantic LFE DirectSpeakers channels when rendering the built-in
// 22.2 (9+10+3) layout. Other layouts retain their existing backend routing.
enum class LfeRoutingMode {
    direct,      // LFE1 -> ch3, LFE2/LFER -> ch9, unity gain
    split_power, // one semantic LFE -> ch3 + ch9 at sqrt(0.5) each
};

enum class AppleSpatialPreset {
    off,
    headphone_default, // AUSpatialMixer factory preset #1: Headphone Media Playback Default
    headphone_movie,   // AUSpatialMixer factory preset #2: Headphone Media Playback Movie
};

// 听者头部朝向：相对默认正前（方位 0）的头部旋转，单位度。yaw 取与 ADM 方位一致的
// 正向约定（+ 向左），pitch + 向上，roll + 向右倾。Apple AUSpatialMixer binaural 后端（映射到
// HeadYaw/Pitch/Roll 全局参数）与 SAF binaural 后端（逐源 HRTF 方向旋转）实装；其它后端忽略。
// 默认全 0 = 头部正前，与未设置完全一致。
struct ListenerOrientation {
    float yaw_deg{0.0F};   // [-180, 180]
    float pitch_deg{0.0F}; // [-90, 90]
    float roll_deg{0.0F};  // [-180, 180]
    [[nodiscard]] bool is_identity() const noexcept { return yaw_deg == 0.0F && pitch_deg == 0.0F && roll_deg == 0.0F; }
};

} // namespace mradm
