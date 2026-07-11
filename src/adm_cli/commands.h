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
    double final_gain_db{0.0};
    std::string output_bit_depth_str{"f32"};
    float loudness_target{std::numeric_limits<float>::quiet_NaN()};
    double render_start{0.0};
    double render_end{std::numeric_limits<double>::quiet_NaN()};
    uint32_t interp_ms{5};
    uint32_t object_smoothing_frames{0};
    uint32_t opus_bitrate_per_ch{0};
    uint32_t apac_bitrate{0};
    bool apac_drc_music{true};
    std::string apac_container_str{"mpeg4"};
    std::string sofa_path;
    std::string semantic_policy_path;
    std::string semantic_report_path;
    std::string speaker_spread_mode_str{"auto"};
    std::string binaural_spread_mode_str{"auto"};
    std::string apple_spatial_preset_str{"off"};
    bool apple_speaker_rendering_flags{false};
    // Listener head orientation in degrees (binaural backends — Apple & SAF). NaN = unset → 0.
    double listener_yaw{std::numeric_limits<double>::quiet_NaN()};
    double listener_pitch{std::numeric_limits<double>::quiet_NaN()};
    double listener_roll{std::numeric_limits<double>::quiet_NaN()};
    std::string iamf_container_str{"obu"};
    std::string iamf_layers_csv;
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

struct ExportCliOptions {
    std::string input;
    std::string output;
    std::string semantic_policy_path;
};

CLI::App* add_render_command(CLI::App& app, RenderCliOptions& opts);
int run_render(const RenderCliOptions& opts);

CLI::App* add_inspect_command(CLI::App& app, InspectCliOptions& opts);
int run_inspect(const InspectCliOptions& opts);

CLI::App* add_backends_command(CLI::App& app);
void run_backends();

CLI::App* add_layouts_command(CLI::App& app, LayoutCliOptions& opts);
int run_layouts(const LayoutCliOptions& opts);

CLI::App* add_formats_command(CLI::App& app);
void run_formats();

CLI::App* add_export_command(CLI::App& app, ExportCliOptions& opts);
int run_export(const ExportCliOptions& opts);

// Hidden `__apac-encode` worker subcommand: the engine forks `mradm __apac-encode`
// to run the AudioToolbox APAC encoder in an isolated, kill-able process behind a
// stall watchdog. Not a user-facing command.
struct ApacEncodeCliOptions {
    std::string input;
    std::string output;
    std::string layout;
    uint32_t bitrate{0};
    bool drc_music{true};
    std::string container{"mpeg4"};
};

CLI::App* add_apac_encode_command(CLI::App& app, ApacEncodeCliOptions& opts);
int run_apac_encode(const ApacEncodeCliOptions& opts);
