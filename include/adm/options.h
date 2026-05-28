#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mradm {

enum class OutputBitDepth {
    f32, // WAVE_FORMAT_IEEE_FLOAT — default; preserves headroom above 0 dBFS
    i24, // 24-bit integer PCM
    i16, // 16-bit integer PCM
};

enum class RendererSelection {
    automatic,
    ear,
    saf,
    hoa,
    apple,
    binaural,
};

enum class SpeakerSpreadMode {
    automatic, // mdap for 3D layouts, none for 2D
    none,      // disable MDAP; Objects always use point VBAP regardless of extent
    mdap,      // always use MDAP (multi-directional amplitude panning) for 3D layouts
};

enum class BinauralSpreadMode {
    automatic,    // cloud
    none,         // point source; extent parameters ignored
    cloud,        // 17-point angular extent cloud (default)
    saf_spreader, // experimental: SAF spreader OM mode (covariance-matching STFT domain)
};

struct RenderOptions {
    RendererSelection renderer{RendererSelection::automatic};
    std::string output_layout{"0+2+0"};
    bool measure_loudness{false};       // opt-in: loudness norm applies global gain
    float loudness_target_lufs{-23.0F}; // EBU R128 broadcast standard
    bool peak_limit{true};
    float peak_limit_dbtp{-1.0F}; // True Peak target in dBTP (broadcast standard)
    // Opt-in peak makeup: after loudness adjustment, raise global gain up to
    // peak_limit_dbtp when measured True Peak is below the ceiling.
    bool peak_normalize_to_limit{false};
    OutputBitDepth output_bit_depth{OutputBitDepth::f32};
    // Opus MKA output: target bitrate per channel in kbps (VBR hint).
    // 0 = auto (64 kbps/ch; minimum 128 kbps for stereo).
    uint32_t opus_bitrate_per_ch_kbps{0};
    // APAC output (macOS only): total target/hint bitrate in kbps.
    // The encoder may produce a measured average bitrate that differs substantially.
    // 0 = layout default for spatial/HOA layouts, otherwise encoder default.
    uint32_t apac_bitrate_kbps{0};
    // APAC DRC profile: true = Music (cdrc=1), false = None (cdrc=0).
    bool apac_drc_music{true};
    // User SOFA HRIR file for the binaural renderer. Empty = built-in KEMAR.
    std::optional<std::filesystem::path> sofa_path;
    // Optional ADM semantic render policy JSON. Applied to the imported scene
    // before handing metadata to the selected renderer.
    std::optional<std::filesystem::path> semantic_policy_path;
    // Optional effective semantic report JSON for debugging policy matches.
    std::optional<std::filesystem::path> semantic_report_path;
    // Default gain-interpolation ramp used when jumpPosition=false and the ADM block
    // carries no explicit interpolationLength.  Set to 0 for instant switching.
    uint32_t default_interp_ms{5};
    // De-zipper window for rapidly changing Objects metadata, in sample frames.
    // 0 disables renderer-side control-rate smoothing and follows ADM blocks sample-accurately.
    uint32_t object_smoothing_frames{8875};
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    BinauralSpreadMode binaural_spread_mode{BinauralSpreadMode::automatic};
    // Internal diagnostics/tests only. The CLI never exposes this; normal users
    // cannot request speaker-stereo ADM rendering.
    bool internal_allow_speaker_stereo{false};
};

struct RenderRequest {
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> output_path;
    RenderOptions options;
};

} // namespace mradm
