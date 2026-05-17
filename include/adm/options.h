#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mradm {

enum class RendererSelection {
    automatic,
    ear,
    saf,
    apple,
};

struct RenderOptions {
    RendererSelection renderer{RendererSelection::automatic};
    std::string output_layout{"binaural"};
    bool measure_loudness{true};
    bool peak_limit{true};
};

struct RenderRequest {
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> output_path;
    RenderOptions options;
};

} // namespace mradm
