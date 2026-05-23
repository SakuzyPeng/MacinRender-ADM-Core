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

struct RenderOptions {
    RendererSelection renderer{RendererSelection::automatic};
    std::string output_layout{"0+2+0"};
    bool measure_loudness{false};       // opt-in: loudness norm applies global gain
    float loudness_target_lufs{-23.0F}; // EBU R128 broadcast standard
    bool peak_limit{true};
    float peak_limit_dbtp{-1.0F}; // True Peak target in dBTP (broadcast standard)
    OutputBitDepth output_bit_depth{OutputBitDepth::f32};
    // Opus MKA output: target bitrate per channel in kbps (VBR hint).
    // 0 = auto (64 kbps/ch; minimum 128 kbps for stereo).
    uint32_t opus_bitrate_per_ch_kbps{0};
    // APAC output (macOS only): total target/hint bitrate in kbps.
    // The encoder may produce a measured average bitrate that differs substantially.
    // 0 = encoder default (~1 Mbps for 7.1 @ 48 kHz).
    uint32_t apac_bitrate_kbps{0};
    // APAC DRC profile: true = Music (cdrc=1), false = None (cdrc=0).
    bool apac_drc_music{true};
    // User SOFA HRIR file for the binaural renderer. Empty = built-in KEMAR.
    std::optional<std::filesystem::path> sofa_path;
    // Default gain-interpolation ramp used when jumpPosition=false and the ADM block
    // carries no explicit interpolationLength.  Set to 0 for instant switching.
    uint32_t default_interp_ms{5};
};

struct RenderRequest {
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> output_path;
    RenderOptions options;
};

} // namespace mradm
