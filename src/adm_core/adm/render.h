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

// Restricts a backend's inline loudness / True-Peak measurement to a frame
// sub-range of the rendered output timeline. Backends still render and write the
// full timeline (so decorrelator / delay state stays continuous); only the meter
// is fed [start_frame, start_frame + frame_count). RenderService sets this when an
// output trim is requested so the reported metrics describe the kept segment.
struct MeterWindow {
    uint64_t start_frame{0};
    uint64_t frame_count{0};
};

// Requests that a backend render ONLY the output frame window
// [start_frame, start_frame + frame_count) instead of the full timeline. A backend
// that advertises CapabilityReport::supports_render_window seeks its input and runs
// an internal warm-up pre-roll (enough to converge any finite-memory DSP state:
// decorrelator overlap, compensation delay, etc.), then writes exactly the window —
// the output file is already trimmed and the inline meter measures only the window,
// bit-identical to a full render followed by trim_file_frames. Backends that do not
// support it ignore this field; RenderService then renders the full timeline and
// trims afterward. Frames are on the output timeline, which equals the input
// timeline. Set by RenderService in place of meter_window when the chosen backend
// supports it.
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
    std::optional<std::filesystem::path> sofa_path; // binaural renderer only; empty = built-in KEMAR
    // Default gain-interpolation ramp in milliseconds (from RenderOptions).
    uint32_t default_interp_ms{5};
    // Renderer-side control-rate smoothing for dense Objects metadata updates.
    // 0 disables smoothing. Backend-dependent: the Apple AUSpatialMixer backend
    // currently ignores this option and relies on SpatialMixer's internal smoothing.
    uint32_t object_smoothing_frames{0};
    // Effective output-speaker coordinates for software speaker renderers.
    SpeakerGeometry speaker_geometry{SpeakerGeometry::standard};
    DirectSpeakersRoutingMode direct_speakers_routing_mode{DirectSpeakersRoutingMode::automatic};
    // Parsed and scene-validated sparse routing matrix. Non-null exactly when
    // direct_speakers_routing_mode == matrix.
    std::shared_ptr<const DirectSpeakersMatrix> direct_speakers_matrix;
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    TripleBalanceMode triple_balance_mode{TripleBalanceMode::standard};
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
    // When set, restrict loudness / True-Peak measurement to this output frame
    // window (matches the output trim); nullopt measures the whole render.
    std::optional<MeterWindow> meter_window;
    // When set, render only this output sub-window with internal pre-roll instead of
    // the full timeline (backends advertising supports_render_window). Mutually
    // exclusive with meter_window: when render_window is set the backend meters
    // exactly the frames it writes (the window).
    std::optional<RenderWindow> render_window;
    // Cooperative cancellation token (copied from RenderOptions by RenderService).
    // Backends check cancel_token.stop_requested() at chunk boundaries and return
    // ErrorCode::cancelled when a stop is requested. A default-constructed token
    // never stops.
    std::stop_token cancel_token;
    // Optional diagnostics from prepare(), including rejected combinations.
    // Invoked synchronously; never retained in reusable prepared/DSP state.
    std::function<void(std::string)> renderer_semantics_sink{};
    RoomConfig room{};
    ScreenConfig screens{};
};

} // namespace mradm
