#pragma once

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

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
    saf_binaural, // SAF HRTF binaural backend
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

enum class AppleSpatialPreset {
    off,
    headphone_default, // AUSpatialMixer factory preset #1: Headphone Media Playback Default
    headphone_movie,   // AUSpatialMixer factory preset #2: Headphone Media Playback Movie
};

// 听者头部朝向：相对默认正前（方位 0）的头部旋转，单位度。yaw 取与 ADM 方位一致的
// 正向约定（+ 向左），pitch + 向上，roll + 向右倾。目前仅 Apple AUSpatialMixer binaural
// 后端实装（映射到 HeadYaw/Pitch/Roll 全局参数）；其它后端忽略。默认全 0 = 头部正前，
// 与未设置完全一致。
struct ListenerOrientation {
    float yaw_deg{0.0F};   // [-180, 180]
    float pitch_deg{0.0F}; // [-90, 90]
    float roll_deg{0.0F};  // [-180, 180]
    [[nodiscard]] bool is_identity() const noexcept { return yaw_deg == 0.0F && pitch_deg == 0.0F && roll_deg == 0.0F; }
};

struct RenderOptions {
    RendererSelection renderer{RendererSelection::automatic};
    std::string output_layout{"0+2+0"};
    // Monitor-only: route the multichannel monitor output through the macOS system Spatial
    // Audio stack (AVSampleBufferAudioRenderer) for system HRTF + dynamic head tracking,
    // instead of folding to a stereo downmix on a raw device. Requires a supported multichannel
    // speaker output_layout (5.1 … 22.2); ignored off-macOS and by offline rendering.
    bool monitor_system_spatial{false};
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
    // APAC output container. MPEG-4 is selected by default for .m4a/.mp4; CAF is
    // opt-in so ordinary .caf output remains float32 PCM unless explicitly requested.
    enum class ApacContainer : uint8_t { mpeg4, caf };
    ApacContainer apac_container{ApacContainer::mpeg4};
    // User SOFA HRIR file for the binaural renderer. Empty = built-in KEMAR.
    std::optional<std::filesystem::path> sofa_path;
    // Optional ADM semantic render policy JSON. Applied to the imported scene
    // before handing metadata to the selected renderer.
    std::optional<std::filesystem::path> semantic_policy_path;
    // Optional in-memory semantic policy JSON (UTF-8). When set it takes precedence
    // over semantic_policy_path, letting a GUI apply an edited policy without a temp
    // file. nullopt = no in-memory policy.
    std::optional<std::string> semantic_policy_json;
    // Optional effective semantic report JSON for debugging policy matches.
    std::optional<std::filesystem::path> semantic_report_path;
    // When true, the effective semantic report is also captured in-memory and
    // returned via RenderResult::semantic_report_json (independent of whether
    // semantic_report_path requests a file copy).
    bool capture_semantic_report{false};
    // Default gain-interpolation ramp used when jumpPosition=false and the ADM block
    // carries no explicit interpolationLength.  Set to 0 for instant switching.
    uint32_t default_interp_ms{5};
    // De-zipper window for rapidly changing Objects metadata, in sample frames.
    // 0 disables renderer-side control-rate smoothing and follows ADM blocks sample-accurately.
    // Backend-dependent: the Apple AUSpatialMixer backend currently ignores this
    // option and relies on SpatialMixer's internal parameter smoothing.
    uint32_t object_smoothing_frames{0};
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    BinauralSpreadMode binaural_spread_mode{BinauralSpreadMode::automatic};
    // Apple AUSpatialMixer binaural only. PresentPreset resets several unit/bus
    // parameters, so the Apple backend applies it before restoring ADM-driven setup.
    AppleSpatialPreset apple_spatial_preset{AppleSpatialPreset::off};
    // 听者头部朝向。Apple binaural 后端（HeadYaw/Pitch/Roll）与 SAF binaural 后端（逐源 HRTF 方向旋转）
    // 实装；其它后端忽略。
    ListenerOrientation listener_orientation{};
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
    // Optional IAMF scalable channel layer list. Empty = preserve the current single-layer
    // output_layout behavior. When set, RenderService validates the list for IAMF outputs.
    std::vector<std::string> iamf_layers;
    // Internal diagnostics/tests only. The CLI never exposes this; normal users
    // cannot request speaker-stereo ADM rendering.
    bool internal_allow_speaker_stereo{false};
    // Cooperative cancellation. A default-constructed token has no associated
    // stop-state, so stop_requested() is always false (renders never cancel).
    // When a live token is supplied (via the C ABI cancel-token object or a
    // std::stop_source), requesting a stop makes the active render abort at the
    // next chunk/stage boundary and return ErrorCode::cancelled. The token only
    // references shared state, so copying RenderOptions stays cheap and safe.
    std::stop_token cancel_token;
};

struct RenderRequest {
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> output_path;
    RenderOptions options;
};

} // namespace mradm
