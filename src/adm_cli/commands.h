#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include <CLI/CLI.hpp>

struct RenderCliOptions {
    std::string input;
    std::string output;
    std::string layout{"0+2+0"};
    std::string renderer{"auto"};
    bool no_peak_limit{false};
    float peak_limit_dbtp{-1.0F};
    bool peak_normalize_to_limit{false};
    std::string output_bit_depth_str{"f32"};
    float loudness_target{std::numeric_limits<float>::quiet_NaN()};
    uint32_t interp_ms{5};
    uint32_t object_smoothing_frames{8875};
    uint32_t opus_bitrate_per_ch{0};
    uint32_t apac_bitrate{0};
    bool apac_drc_music{true};
    std::string sofa_path;
    std::string semantic_policy_path;
    std::string semantic_report_path;
    std::string speaker_spread_mode_str{"auto"};
    std::string binaural_spread_mode_str{"auto"};
};

struct InspectCliOptions {
    std::string input;
    bool xml{false};
    std::string semantic_policy_template_path;
};

struct LayoutCliOptions {
    std::string format;
    std::string layout;
    std::string renderer;
};

CLI::App* add_render_command(CLI::App& app, RenderCliOptions& opts);
int run_render(const RenderCliOptions& opts);

CLI::App* add_inspect_command(CLI::App& app, InspectCliOptions& opts);
int run_inspect(const InspectCliOptions& opts);

CLI::App* add_backends_command(CLI::App& app);
void run_backends();

CLI::App* add_layouts_command(CLI::App& app, LayoutCliOptions& opts);
int run_layouts(const LayoutCliOptions& opts);
