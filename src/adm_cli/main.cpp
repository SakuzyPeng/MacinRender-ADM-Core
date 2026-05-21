#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"
#include "adm/version.h"

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

void print_loudness(const mradm::SceneLoudnessMetadata& lm) {
    if (lm.loudness_method) {
        fmt::print("    loudness method: {}\n", *lm.loudness_method);
    }
    if (lm.integrated_loudness) {
        fmt::print("    integrated loudness: {:.1f} LUFS\n", *lm.integrated_loudness);
    }
    if (lm.max_true_peak) {
        fmt::print("    max true peak:       {:.1f} dBTP\n", *lm.max_true_peak);
    }
    if (lm.loudness_range) {
        fmt::print("    loudness range:      {:.1f} LU\n", *lm.loudness_range);
    }
    if (lm.max_momentary) {
        fmt::print("    max momentary:       {:.1f} LUFS\n", *lm.max_momentary);
    }
    if (lm.max_short_term) {
        fmt::print("    max short-term:      {:.1f} LUFS\n", *lm.max_short_term);
    }
    if (lm.dialogue_loudness) {
        fmt::print("    dialogue loudness:   {:.1f} LUFS\n", *lm.dialogue_loudness);
    }
}

void print_programmes(const std::vector<mradm::SceneProgramme>& programmes) {
    fmt::print("\nProgrammes ({}):\n", programmes.size());
    for (const auto& p : programmes) {
        fmt::print("  {}  \"{}\"\n", p.id, p.name);
        if (p.has_reference_screen) {
            fmt::print("    reference screen: present (geometry not parsed by libadm)\n");
        }
        if (p.loudness) {
            print_loudness(*p.loudness);
        }
        for (const auto& cid : p.content_ids) {
            fmt::print("    → {}\n", cid);
        }
    }
}

void print_content_metadata(const mradm::SceneContent& c) {
    if (c.language) {
        fmt::print("    language: {}\n", *c.language);
    }
    if (!c.labels.empty()) {
        fmt::print("    label:");
        for (const auto& l : c.labels) {
            fmt::print(" \"{}\"", l);
        }
        fmt::print("\n");
    }
    if (c.dialogue_kind) {
        if (c.content_kind) {
            fmt::print("    dialogue: {} ({})\n", *c.dialogue_kind, *c.content_kind);
        } else {
            fmt::print("    dialogue: {}\n", *c.dialogue_kind);
        }
    }
    if (c.loudness) {
        print_loudness(*c.loudness);
    }
}

void print_contents(const std::vector<mradm::SceneContent>& contents) {
    fmt::print("\nContents ({}):\n", contents.size());
    for (const auto& c : contents) {
        fmt::print("  {}  \"{}\"\n", c.id, c.name);
        print_content_metadata(c);
        for (const auto& oid : c.object_ids) {
            fmt::print("    → {}\n", oid);
        }
    }
}

void print_scene(const std::string& path, const mradm::AdmScene& scene) {
    const auto& info = scene.info;
    fmt::print("File: {}\n", path);
    fmt::print(
        "  Sample rate: {} Hz  Channels: {}  Frames: {}\n", info.sample_rate, info.num_channels, info.num_frames);
    if (info.sample_rate > 0 && info.num_frames > 0) {
        fmt::print("  Duration:    {:.2f} s\n", static_cast<double>(info.num_frames) / info.sample_rate);
    }

    print_programmes(scene.programmes);
    print_contents(scene.contents);

    fmt::print("\nObjects ({}):\n", scene.objects.size());
    for (const auto& obj : scene.objects) {
        fmt::print("  {}  \"{}\"\n", obj.id, obj.name);
        for (const auto& track : obj.tracks) {
            if (track.channel_index.has_value()) {
                fmt::print("    {}  ch={}\n", track.track_uid, *track.channel_index);
            } else {
                fmt::print("    {}  ch=<unmapped>\n", track.track_uid);
            }
            for (std::size_t bi = 0; bi < track.blocks.size(); ++bi) {
                const auto& blk = track.blocks[bi];
                if (blk.position.cartesian) {
                    fmt::print("      block[{}]: x={:.3f} y={:.3f} z={:.3f}  gain={:.3f}\n",
                               bi,
                               blk.position.x,
                               blk.position.y,
                               blk.position.z,
                               blk.gain);
                } else {
                    fmt::print("      block[{}]: az={:.1f} el={:.1f} dist={:.3f}  gain={:.3f}\n",
                               bi,
                               blk.position.azimuth,
                               blk.position.elevation,
                               blk.position.distance,
                               blk.gain);
                }
                if (blk.diffuse > 0.0F || blk.width > 0.0F || blk.height > 0.0F) {
                    fmt::print("               diffuse={:.3f}  w={:.1f} h={:.1f} d={:.1f}\n",
                               blk.diffuse,
                               blk.width,
                               blk.height,
                               blk.depth);
                }
            }
            for (std::size_t bi = 0; bi < track.ds_blocks.size(); ++bi) {
                const auto& blk = track.ds_blocks[bi];
                fmt::print("      ds_block[{}]: labels=", bi);
                if (blk.speaker_labels.empty()) {
                    fmt::print("<none>");
                } else {
                    for (std::size_t li = 0; li < blk.speaker_labels.size(); ++li) {
                        fmt::print("{}{}", li == 0 ? "" : ",", blk.speaker_labels[li]);
                    }
                }
                fmt::print(
                    "  pack={}  gain={:.3f}\n", blk.pack_format_id.empty() ? "<unset>" : blk.pack_format_id, blk.gain);
                if (blk.has_position) {
                    fmt::print("                   az={:.1f} el={:.1f} dist={:.3f}\n",
                               blk.azimuth,
                               blk.elevation,
                               blk.distance);
                }
            }
        }
    }
}

void print_capabilities(const mradm::CapabilityReport& caps) {
    fmt::print("Backend: {} {}\n", caps.backend_name, caps.backend_version);
    fmt::print("  Objects:        {}\n", caps.supports_objects ? "yes" : "no");
    fmt::print("  DirectSpeakers: {}\n", caps.supports_direct_speakers ? "yes" : "no");
    fmt::print("  HOA:            {}\n", caps.supports_hoa ? "yes" : "no");
    fmt::print("  Layouts ({}):\n", caps.supported_layouts.size());
    for (const auto& layout : caps.supported_layouts) {
        std::string flags;
        if (layout.channel_count > 0) {
            flags += fmt::format("{}ch", layout.channel_count);
        }
        if (layout.lfe_count > 0) {
            flags += fmt::format(" {}lfe", layout.lfe_count);
        }
        if (layout.is_3d) {
            flags += " 3d";
        }
        if (layout.supports_spread) {
            flags += " spread";
        }
        fmt::print("    {:<12}  {:<30}  {}\n", layout.id, layout.display_name, flags);
    }
}

void print_all_capabilities() {
    print_capabilities(mradm::ear_capabilities());
    fmt::print("\n");
    print_capabilities(mradm::vbap_capabilities());
    fmt::print("\n");
    print_capabilities(mradm::hoa_capabilities());
}

struct RenderCliOptions {
    std::string input;
    std::string output;
    std::string layout{"0+2+0"};
    std::string renderer{"auto"};
    bool no_peak_limit{false};
    float peak_limit_dbtp{-1.0F};
    std::string output_bit_depth_str{"f32"};
    float loudness_target{std::numeric_limits<float>::quiet_NaN()};
    uint32_t interp_ms{5};
};

CLI::App* add_render_command(CLI::App& app, RenderCliOptions& opts) {
    auto* render_cmd = app.add_subcommand("render", "Render an ADM BWF file");
    render_cmd->add_option("-i,--input", opts.input, "Input ADM BWF/WAV path")->required();
    render_cmd->add_option("-o,--output", opts.output, "Output audio path");
    render_cmd->add_option("--output-layout", opts.layout, "Output layout identifier");
    render_cmd->add_option("--renderer", opts.renderer, "Renderer backend: auto, ear, saf, hoa, apple")
        ->check(CLI::IsMember({"auto", "ear", "saf", "hoa", "apple"}));
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
    if (!std::isnan(opts.loudness_target)) {
        request.options.measure_loudness = true;
        request.options.loudness_target_lufs = opts.loudness_target;
    }
    return request;
}

int run_render(const RenderCliOptions& opts) {
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

int main(int argc, char** argv) {
    CLI::App app{"MacinRender ADM Core command-line interface"};
    app.set_version_flag("--version", fmt::format("adm {}", mradm::version()));

    // ── render ────────────────────────────────────────────────────────────────
    RenderCliOptions render_opts;
    bool verbose{false};
    auto* render_cmd = add_render_command(app, render_opts);
    render_cmd->add_flag("-v,--verbose", verbose, "Enable verbose logs");

    // ── inspect ───────────────────────────────────────────────────────────────
    std::string inspect_input;
    bool inspect_xml{false};
    auto* inspect_cmd = app.add_subcommand("inspect", "Print ADM scene metadata from a BWF file");
    inspect_cmd->add_option("file", inspect_input, "ADM BWF/WAV input path")->required();
    inspect_cmd->add_flag("--xml", inspect_xml, "Dump raw AXML chunk instead of parsed summary");

    // ── backends ──────────────────────────────────────────────────────────────
    auto* backends_cmd = app.add_subcommand("backends", "List available renderer backends and supported layouts");

    app.require_subcommand(1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    spdlog::set_pattern("%^[%l]%$ %v");
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);

    if (*render_cmd) {
        return run_render(render_opts);
    }

    if (*inspect_cmd) {
        if (inspect_xml) {
            auto result = mradm::io::get_axml(inspect_input);
            if (!result.has_value()) {
                spdlog::error("{}", result.error().message);
                return EXIT_FAILURE;
            }
            fmt::print("{}", result.value());
        } else {
            auto result = mradm::io::import_scene(inspect_input);
            if (!result.has_value()) {
                spdlog::error("{}", result.error().message);
                return EXIT_FAILURE;
            }
            print_scene(inspect_input, result.value());
        }
    }

    if (*backends_cmd) {
        print_all_capabilities();
    }

    return EXIT_SUCCESS;
}
