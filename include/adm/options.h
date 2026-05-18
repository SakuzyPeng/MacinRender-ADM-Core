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
};

struct RenderOptions {
    RendererSelection renderer{RendererSelection::automatic};
    std::string output_layout{"binaural"};
    bool measure_loudness{false};       // opt-in: loudness norm applies global gain
    float loudness_target_lufs{-23.0F}; // EBU R128 broadcast standard
    bool peak_limit{true};
    float peak_limit_dbtp{-1.0F}; // True Peak target in dBTP (broadcast standard)
    OutputBitDepth output_bit_depth{OutputBitDepth::f32};
};

struct RenderRequest {
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> output_path;
    RenderOptions options;
};

} // namespace mradm
