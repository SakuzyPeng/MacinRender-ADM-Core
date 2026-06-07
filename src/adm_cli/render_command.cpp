#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "adm/render.h"

#include "commands.h"

namespace {

constexpr int k_sigint_exit_code = 130;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): required by the C signal handler contract.
volatile std::sig_atomic_t g_sigint_requested = 0;

void handle_sigint(int /*signal*/) noexcept {
    g_sigint_requested = 1;
}

using SignalHandler = void (*)(int);

SignalHandler install_sigint_handler() {
    g_sigint_requested = 0;
    return std::signal(SIGINT, handle_sigint);
}

class RenderInterruptScope {
  public:
    explicit RenderInterruptScope(std::stop_source& stop_source)
        : stop_source_(stop_source), previous_handler_(install_sigint_handler()) {
        if (previous_handler_ == SIG_ERR) {
            spdlog::warn("Ctrl-C cancellation is unavailable: failed to install SIGINT handler");
            return;
        }
        watcher_ = std::thread([this] {
            while (!done_.load(std::memory_order_acquire)) {
                if (g_sigint_requested > 0) {
                    spdlog::info("Ctrl-C received; cancelling render...");
                    stop_source_.request_stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{25});
            }
        });
    }

    RenderInterruptScope(const RenderInterruptScope&) = delete;
    RenderInterruptScope& operator=(const RenderInterruptScope&) = delete;
    RenderInterruptScope(RenderInterruptScope&&) = delete;
    RenderInterruptScope& operator=(RenderInterruptScope&&) = delete;

    ~RenderInterruptScope() {
        done_.store(true, std::memory_order_release);
        if (watcher_.joinable()) {
            watcher_.join();
        }
        if (previous_handler_ != SIG_ERR) {
            std::signal(SIGINT, previous_handler_);
        }
    }

  private:
    std::stop_source& stop_source_;
    std::atomic_bool done_{false};
    std::thread watcher_;
    void (*previous_handler_)(int){SIG_ERR};
};

class SpdlogSink final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view module, std::string_view message) override {
        const auto text = std::string{"["} + std::string{module} + "] " + std::string{message};
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
        spdlog::debug(std::string{"progress "} + std::to_string(static_cast<int>(std::lround(event.fraction * 100.0))) +
                      "%: " + std::string{event.message});
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
    if (value == "saf-binaural") {
        return mradm::RendererSelection::saf_binaural;
    }
    if (value == "binaural") {
        return mradm::RendererSelection::binaural;
    }
    return mradm::RendererSelection::automatic;
}

CLI::Validator renderer_validator() {
    return CLI::Validator{[](const std::string& value) {
                              if (value == "auto" || value == "ear" || value == "saf" || value == "hoa" ||
                                  value == "saf-binaural" || value == "apple" || value == "binaural") {
                                  return std::string{};
                              }
                              return std::string{"expected one of: auto, ear, saf, hoa, saf-binaural, apple"};
                          },
                          "auto,ear,saf,hoa,saf-binaural,apple",
                          "renderer"};
}

mradm::SpeakerSpreadMode parse_speaker_spread_mode(const std::string& value) {
    if (value == "none") {
        return mradm::SpeakerSpreadMode::none;
    }
    if (value == "mdap") {
        return mradm::SpeakerSpreadMode::mdap;
    }
    return mradm::SpeakerSpreadMode::automatic;
}

mradm::BinauralSpreadMode parse_binaural_spread_mode(const std::string& value) {
    if (value == "none") {
        return mradm::BinauralSpreadMode::none;
    }
    if (value == "cloud") {
        return mradm::BinauralSpreadMode::cloud;
    }
    if (value == "saf-spreader") {
        return mradm::BinauralSpreadMode::saf_spreader;
    }
    return mradm::BinauralSpreadMode::automatic;
}

mradm::AppleSpatialPreset parse_apple_spatial_preset(const std::string& value) {
    if (value == "headphone-default") {
        return mradm::AppleSpatialPreset::headphone_default;
    }
    if (value == "headphone-movie") {
        return mradm::AppleSpatialPreset::headphone_movie;
    }
    return mradm::AppleSpatialPreset::off;
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

mradm::RenderOptions::ApacContainer parse_apac_container(const std::string& value) {
    if (value == "caf") {
        return mradm::RenderOptions::ApacContainer::caf;
    }
    return mradm::RenderOptions::ApacContainer::mpeg4;
}

CLI::App* add_render_command_impl(CLI::App& app, RenderCliOptions& opts) {
    auto* render_cmd = app.add_subcommand("render", "Render an ADM BWF file");
    render_cmd->add_option("-i,--input", opts.input, "Input ADM BWF/WAV path")->required();
    render_cmd->add_option("-o,--output", opts.output, "Output audio path");
    render_cmd->add_option("--output-layout",
                           opts.layout,
                           "Output layout for non-binaural renderers; use 'mradm layouts --format <fmt>' for final "
                           "container channel order");
    render_cmd->add_option("--renderer", opts.renderer, "Renderer backend: auto, ear, saf, hoa, saf-binaural, apple")
        ->check(renderer_validator());
    render_cmd->add_flag("--no-peak-limit", opts.no_peak_limit, "Disable True Peak limiting");
    render_cmd->add_option("--peak-limit-dbtp", opts.peak_limit_dbtp, "True Peak target in dBTP")
        ->check(CLI::Range(-60.0F, 0.0F));
    render_cmd->add_flag("--peak-normalize-to-limit",
                         opts.peak_normalize_to_limit,
                         "Raise global gain up to --peak-limit-dbtp when measured True Peak is below the ceiling");
    render_cmd->add_option("--final-gain-db",
                           opts.final_gain_db,
                           "Unconstrained final gain in dB, applied after loudness/peak staging; bypasses peak "
                           "limiting and may exceed 0 dBFS (default: 0)");
    render_cmd->add_option("--output-bit-depth", opts.output_bit_depth_str, "Output bit depth: f32, i24, i16")
        ->check(CLI::IsMember({"f32", "i24", "i16"}));
    render_cmd
        ->add_option("--loudness-target",
                     opts.loudness_target,
                     "Normalise integrated loudness to this LUFS target (enables loudness normalisation)")
        ->check(CLI::Range(-70.0F, 0.0F));
    render_cmd
        ->add_option("--start",
                     opts.render_start,
                     "Trim output to start at this time in seconds (default: 0 = from the beginning)")
        ->check(CLI::NonNegativeNumber);
    render_cmd->add_option(
        "--end",
        opts.render_end,
        "Trim output to end at this absolute time in seconds (default: render to the end); must be > --start. "
        "Loudness/True-Peak are measured over the trimmed segment");
    render_cmd
        ->add_option("--interp-ms",
                     opts.interp_ms,
                     "Default gain-interpolation ramp in ms when ADM block has no jumpPosition/interpolationLength "
                     "(default: 5, set to 0 for instant switching)")
        ->check(CLI::Range(0U, 500U));
    render_cmd
        ->add_option("--object-smoothing-frames",
                     opts.object_smoothing_frames,
                     "De-zipper window for dense Objects metadata updates in sample frames "
                     "(default: 0 for sample-accurate block switching; "
                     "currently ignored by the apple backend)")
        ->check(CLI::Range(0U, 48000U));
    render_cmd
        ->add_option("--opus-bitrate-per-ch",
                     opts.opus_bitrate_per_ch,
                     "Opus MKA VBR target bitrate per channel in kbps (6-320); "
                     "omit for auto: 64 kbps/ch, 128 kbps floor for mono/stereo")
        ->check(CLI::Range(6U, 320U));
    render_cmd
        ->add_option("--apac-bitrate",
                     opts.apac_bitrate,
                     "APAC total target/hint bitrate in kbps (64-32768); "
                     "actual measured bitrate may differ substantially; omit for layout default")
        ->check(CLI::Range(64U, 32768U));
    render_cmd->add_flag("--apac-drc-none{false},--apac-drc-music{true}",
                         opts.apac_drc_music,
                         "APAC DRC profile: --apac-drc-music (default) or --apac-drc-none");
    render_cmd
        ->add_option("--apac-container",
                     opts.apac_container_str,
                     "APAC output container: mpeg4 (.m4a/.mp4, default) or caf (.caf)")
        ->check(CLI::IsMember({"mpeg4", "caf"}));
    render_cmd->add_option("--sofa", opts.sofa_path, "User SOFA HRIR file for binaural rendering");
    render_cmd->add_option("--semantic-policy", opts.semantic_policy_path, "ADM semantic render policy JSON");
    render_cmd->add_option("--write-semantic-report",
                           opts.semantic_report_path,
                           "Write effective ADM semantic report JSON after applying policy");
    render_cmd
        ->add_option("--speaker-spread-mode",
                     opts.speaker_spread_mode_str,
                     "Speaker Objects extent spread algorithm: auto (mdap for 3D, none for 2D), none, mdap")
        ->check(CLI::IsMember({"auto", "none", "mdap"}));
    render_cmd
        ->add_option("--binaural-spread-mode",
                     opts.binaural_spread_mode_str,
                     "Binaural Objects extent spread algorithm: auto (cloud), none, cloud, "
                     "saf-spreader [experimental: SAF covariance-matching STFT-domain spreader]")
        ->check(CLI::IsMember({"auto", "none", "cloud", "saf-spreader"}));
    render_cmd
        ->add_option("--apple-spatial-preset",
                     opts.apple_spatial_preset_str,
                     "Apple binaural AUSpatialMixer factory preset: off, headphone-default, headphone-movie")
        ->check(CLI::IsMember({"off", "headphone-default", "headphone-movie"}));
    render_cmd
        ->add_option("--iamf-container",
                     opts.iamf_container_str,
                     "IAMF output container [requires MR_ADM_ENABLE_IAMF build]: "
                     "obu (raw .iamf stream, default) or mp4 (ISOBMFF via mp4box/ffmpeg)")
        ->check(CLI::IsMember({"obu", "mp4"}));
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
    request.options.peak_normalize_to_limit = opts.peak_normalize_to_limit;
    request.options.final_gain_db = opts.final_gain_db;
    request.options.output_bit_depth = parse_output_bit_depth(opts.output_bit_depth_str);
    request.options.default_interp_ms = opts.interp_ms;
    request.options.object_smoothing_frames = opts.object_smoothing_frames;
    request.options.opus_bitrate_per_ch_kbps = opts.opus_bitrate_per_ch;
    request.options.apac_bitrate_kbps = opts.apac_bitrate;
    request.options.apac_drc_music = opts.apac_drc_music;
    request.options.apac_container = parse_apac_container(opts.apac_container_str);
    if (!opts.sofa_path.empty()) {
        request.options.sofa_path = opts.sofa_path;
    }
    if (!opts.semantic_policy_path.empty()) {
        request.options.semantic_policy_path = opts.semantic_policy_path;
    }
    if (!opts.semantic_report_path.empty()) {
        request.options.semantic_report_path = opts.semantic_report_path;
    }
    if (!std::isnan(opts.loudness_target)) {
        request.options.measure_loudness = true;
        request.options.loudness_target_lufs = opts.loudness_target;
    }
    request.options.render_start_sec = opts.render_start;
    if (!std::isnan(opts.render_end)) {
        request.options.render_end_sec = opts.render_end;
    }
    request.options.speaker_spread_mode = parse_speaker_spread_mode(opts.speaker_spread_mode_str);
    request.options.binaural_spread_mode = parse_binaural_spread_mode(opts.binaural_spread_mode_str);
    request.options.apple_spatial_preset = parse_apple_spatial_preset(opts.apple_spatial_preset_str);
    request.options.iamf_container = (opts.iamf_container_str == "mp4") ? mradm::RenderOptions::IamfContainer::mp4
                                                                        : mradm::RenderOptions::IamfContainer::obu;
    return request;
}

int run_render_impl(const RenderCliOptions& opts) {
    mradm::RenderService service;
    ConsoleProgressSink progress;
    SpdlogSink logs;
    std::stop_source stop_source;
    mradm::RenderRequest request = make_render_request(opts);
    request.options.cancel_token = stop_source.get_token();
    RenderInterruptScope interrupt_scope{stop_source};
    mradm::RenderResult result = service.render(request, progress, logs);
    if (!result.success()) {
        if (result.error.code == mradm::ErrorCode::cancelled) {
            spdlog::info(result.error.message);
            return k_sigint_exit_code;
        }
        spdlog::error(result.error.message);
        return EXIT_FAILURE;
    }
    if (result.output_path.has_value()) {
        spdlog::info(std::string{"wrote "} + result.output_path->string());
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
