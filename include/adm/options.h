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
    // Unconstrained final gain in dB, applied after all automatic gain staging
    // (loudness normalisation, peak makeup, peak limiting). Intentionally NOT
    // subject to peak limiting: it is added after the peak clamp is computed, so it
    // can push the signal above the peak ceiling and above 0 dBFS (integer outputs
    // may then clip). 0 = no-op. Folded into the applied gain, so reported metrics
    // and file metadata reflect it.
    double final_gain_db{0.0};
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
    // Output time-range trim, in seconds, on the rendered timeline (which equals
    // the input timeline). render_start_sec clips the head; render_end_sec is an
    // absolute end time on the same timeline (nullopt = render to the end).
    // The backend renders the full timeline but measures loudness/True-Peak only
    // over the kept window, so the trimmed file's metrics/metadata describe the
    // segment that is actually written.
    double render_start_sec{0.0};
    std::optional<double> render_end_sec;
    // IAMF output container when MR_ADM_ENABLE_IAMF=ON.
    // obu: raw .iamf OBU stream (default); mp4: ISOBMFF encapsulation via mp4box/ffmpeg.
    // Requires --iamf-container mp4 on the CLI; the engine ignores this field for non-IAMF outputs.
    enum class IamfContainer : uint8_t { obu, mp4 };
    IamfContainer iamf_container{IamfContainer::obu};
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
