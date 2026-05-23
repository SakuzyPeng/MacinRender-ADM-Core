#include <cmath>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/render.h"

#include "commands.h"

namespace {

class SpdlogSink final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view module, std::string_view message) override {
        const auto text = fmt::format("[{}] {}", module, message);
        switch (level) {
        case mradm::LogLevel::debug:
            spdlog::debug(text);
            break;
        case mradm::LogLevel::info:
            spdlog::info(text);
            break;
        case mradm::LogLevel::warning:
            spdlog::warn(text);
            break;
        case mradm::LogLevel::error:
            spdlog::error(text);
            break;
        }
    }
};

class ConsoleProgressSink final : public mradm::ProgressSink {
  public:
    void on_progress(const mradm::ProgressEvent& event) override {
        spdlog::debug("progress {:.0f}%: {}", event.fraction * 100.0, event.message);
    }
};

mradm::RendererSelection parse_renderer(const std::string& value) {
    if (value == "ear") {
        return mradm::RendererSelection::ear;
    }
    if (value == "saf") {
        return mradm::RendererSelection::saf;
    }
    if (value == "hoa") {
        return mradm::RendererSelection::hoa;
    }
    if (value == "apple") {
        return mradm::RendererSelection::apple;
    }
    if (value == "binaural") {
        return mradm::RendererSelection::binaural;
    }
    return mradm::RendererSelection::automatic;
}

mradm::OutputBitDepth parse_output_bit_depth(const std::string& value) {
    if (value == "i24") {
        return mradm::OutputBitDepth::i24;
    }
    if (value == "i16") {
        return mradm::OutputBitDepth::i16;
    }
    return mradm::OutputBitDepth::f32;
}

CLI::App* add_render_command_impl(CLI::App& app, RenderCliOptions& opts) {
    auto* render_cmd = app.add_subcommand("render", "Render an ADM BWF file");
    render_cmd->add_option("-i,--input", opts.input, "Input ADM BWF/WAV path")->required();
    render_cmd->add_option("-o,--output", opts.output, "Output audio path");
    render_cmd->add_option("--output-layout",
                           opts.layout,
                           "Output layout for non-binaural renderers; use 'adm layouts --format <fmt>' for final "
                           "container channel order");
    render_cmd->add_option("--renderer", opts.renderer, "Renderer backend: auto, ear, saf, hoa, binaural, apple")
        ->check(CLI::IsMember({"auto", "ear", "saf", "hoa", "binaural", "apple"}));
    render_cmd->add_flag("--no-peak-limit", opts.no_peak_limit, "Disable True Peak limiting");
    render_cmd->add_option("--peak-limit-dbtp", opts.peak_limit_dbtp, "True Peak target in dBTP")
        ->check(CLI::Range(-60.0F, 0.0F));
    render_cmd->add_option("--output-bit-depth", opts.output_bit_depth_str, "Output bit depth: f32, i24, i16")
        ->check(CLI::IsMember({"f32", "i24", "i16"}));
    render_cmd
        ->add_option("--loudness-target",
                     opts.loudness_target,
                     "Normalise integrated loudness to this LUFS target (enables loudness normalisation)")
        ->check(CLI::Range(-70.0F, 0.0F));
    render_cmd
        ->add_option("--interp-ms",
                     opts.interp_ms,
                     "Default gain-interpolation ramp in ms when ADM block has no jumpPosition/interpolationLength "
                     "(default: 5, set to 0 for instant switching)")
        ->check(CLI::Range(0U, 500U));
    render_cmd
        ->add_option("--opus-bitrate-per-ch",
                     opts.opus_bitrate_per_ch,
                     "Opus MKA VBR target bitrate per channel in kbps (6-320); "
                     "omit for auto: 64 kbps/ch, 128 kbps floor for mono/stereo")
        ->check(CLI::Range(6U, 320U));
    render_cmd
        ->add_option("--apac-bitrate",
                     opts.apac_bitrate,
                     "APAC total VBR target/hint bitrate in kbps (64-2000); "
                     "actual measured bitrate may differ substantially; omit for encoder default")
        ->check(CLI::Range(64U, 2000U));
    render_cmd->add_flag("--apac-drc-none{false},--apac-drc-music{true}",
                         opts.apac_drc_music,
                         "APAC DRC profile: --apac-drc-music (default) or --apac-drc-none");
    render_cmd->add_option("--sofa", opts.sofa_path, "User SOFA HRIR file for binaural rendering");
    return render_cmd;
}

mradm::RenderRequest make_render_request(const RenderCliOptions& opts) {
    mradm::RenderRequest request;
    request.input_path = opts.input;
    if (!opts.output.empty()) {
        request.output_path = opts.output;
    }
    request.options.output_layout = opts.layout;
    request.options.renderer = parse_renderer(opts.renderer);
    request.options.peak_limit = !opts.no_peak_limit;
    request.options.peak_limit_dbtp = opts.peak_limit_dbtp;
    request.options.output_bit_depth = parse_output_bit_depth(opts.output_bit_depth_str);
    request.options.default_interp_ms = opts.interp_ms;
    request.options.opus_bitrate_per_ch_kbps = opts.opus_bitrate_per_ch;
    request.options.apac_bitrate_kbps = opts.apac_bitrate;
    request.options.apac_drc_music = opts.apac_drc_music;
    if (!opts.sofa_path.empty()) {
        request.options.sofa_path = opts.sofa_path;
    }
    if (!std::isnan(opts.loudness_target)) {
        request.options.measure_loudness = true;
        request.options.loudness_target_lufs = opts.loudness_target;
    }
    return request;
}

int run_render_impl(const RenderCliOptions& opts) {
    mradm::RenderService service;
    ConsoleProgressSink progress;
    SpdlogSink logs;
    mradm::RenderResult result = service.render(make_render_request(opts), progress, logs);
    if (!result.success()) {
        spdlog::error(result.error.message);
        return EXIT_FAILURE;
    }
    if (result.output_path.has_value()) {
        spdlog::info("wrote {}", result.output_path->string());
    }
    return EXIT_SUCCESS;
}

} // namespace

CLI::App* add_render_command(CLI::App& app, RenderCliOptions& opts) {
    return add_render_command_impl(app, opts);
}

int run_render(const RenderCliOptions& opts) {
    return run_render_impl(opts);
}
