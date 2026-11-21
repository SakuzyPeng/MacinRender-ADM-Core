#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "adm/capability.h"
#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/progress.h"

namespace mradm {

namespace detail {
// The imported Rust render scene behind RenderPlan::scene; opaque outside the engine.
struct RenderSceneHandle;
} // namespace detail

struct DirectSpeakersMatrix;

// Inline measurement results from the renderer's render loop.
// Both fields are nullopt when the signal is silence or too short for gating.
// For HOA outputs, LUFS is measured via an AllRAD 7.1.4 decode (spatial domain).
// LFE is excluded from LUFS (unused metering channel) but tracked separately for True Peak.
struct RenderMetrics {
    std::optional<double> measured_lufs;      // BS.1770-4 integrated loudness (LUFS)
    std::optional<double> measured_peak_dbtp; // ITU-R BS.1770-4 True Peak (dBTP)
};

// Requests that a backend render ONLY the output frame window
// [start_frame, start_frame + frame_count) instead of the full timeline. A backend
// that advertises CapabilityReport::supports_render_window seeks its input and runs
// an internal warm-up pre-roll (enough to converge any finite-memory DSP state:
// decorrelator overlap, compensation delay, etc.), then writes exactly the window —
// the output file is already trimmed and the inline meter measures only the window,
// bit-identical to a full render followed by the trim pass. Backends that do not
// support it ignore this field; RenderService then renders the full timeline and
// trims afterward. Frames are on the output timeline, which equals the input
// timeline.
struct RenderWindow {
    uint64_t start_frame{0};
    uint64_t frame_count{0};
};

// Input to a renderer backend: file paths, output layout, and the imported scene.
struct RenderPlan {
    std::string input_path;
    std::string output_path;
    std::string output_layout;
    // The policy-applied Rust render scene, populated by RenderService;
    // backends read only this and must not re-parse ADM.
    std::shared_ptr<const detail::RenderSceneHandle> scene_handle;
    DirectSpeakersRoutingMode direct_speakers_routing_mode{DirectSpeakersRoutingMode::automatic};
    // Parsed and scene-validated sparse routing matrix. Non-null exactly when
    // direct_speakers_routing_mode == matrix.
    std::shared_ptr<const DirectSpeakersMatrix> direct_speakers_matrix;
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    BinauralSpreadMode binaural_spread_mode{BinauralSpreadMode::automatic};
    LfeRoutingMode lfe_routing_mode{LfeRoutingMode::direct};
    AppleSpatialPreset apple_spatial_preset{AppleSpatialPreset::off};
    // Apple speaker output only. false writes SpatialMixerRenderingFlags=0;
    // true enables Apple's InterAuralDelay and DistanceAttenuation flags.
    bool apple_speaker_rendering_flags{false};
    // Listener head orientation. The Apple AUSpatialMixer binaural backend (HeadYaw/Pitch/Roll
    // global params) and the SAF binaural backend (per-source HRTF direction rotation) implement
    // it; other backends ignore it.
    ListenerOrientation listener_orientation{};
    // When set, render only this output sub-window with internal pre-roll instead of
    // the full timeline (backends advertising supports_render_window); the backend then
    // meters exactly the frames it writes (the window).
    std::optional<RenderWindow> render_window;
    // Cooperative cancellation token.
    // Backends check cancel_token.stop_requested() at chunk boundaries and return
    // ErrorCode::cancelled when a stop is requested. A default-constructed token
    // never stops.
    std::stop_token cancel_token;
};

} // namespace mradm
