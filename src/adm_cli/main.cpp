#include <cstdlib>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_ear.h"
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
    if (value == "apple") {
        return mradm::RendererSelection::apple;
    }
    return mradm::RendererSelection::automatic;
}

void print_scene(const std::string& path, const mradm::AdmScene& scene) {
    const auto& info = scene.info;
    fmt::print("File: {}\n", path);
    fmt::print("  Sample rate: {} Hz  Channels: {}  Frames: {}\n",
               info.sample_rate, info.num_channels, info.num_frames);
    if (info.sample_rate > 0 && info.num_frames > 0) {
        fmt::print("  Duration:    {:.2f} s\n",
                   static_cast<double>(info.num_frames) / info.sample_rate);
    }

    fmt::print("\nProgrammes ({}):\n", scene.programmes.size());
    for (const auto& p : scene.programmes) {
        fmt::print("  {}  \"{}\"\n", p.id, p.name);
        for (const auto& cid : p.content_ids) {
            fmt::print("    → {}\n", cid);
        }
    }

    fmt::print("\nContents ({}):\n", scene.contents.size());
    for (const auto& c : scene.contents) {
        fmt::print("  {}  \"{}\"\n", c.id, c.name);
        for (const auto& oid : c.object_ids) {
            fmt::print("    → {}\n", oid);
        }
    }

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
                               bi, blk.position.x, blk.position.y, blk.position.z, blk.gain);
                } else {
                    fmt::print("      block[{}]: az={:.1f} el={:.1f} dist={:.3f}  gain={:.3f}\n",
                               bi, blk.position.azimuth, blk.position.elevation,
                               blk.position.distance, blk.gain);
                }
                if (blk.diffuse > 0.0f || blk.width > 0.0f || blk.height > 0.0f) {
                    fmt::print("               diffuse={:.3f}  w={:.1f} h={:.1f} d={:.1f}\n",
                               blk.diffuse, blk.width, blk.height, blk.depth);
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
        fmt::print("    {:<12}  {}\n", layout.id, layout.display_name);
    }
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"MacinRender ADM Core command-line interface"};
    app.set_version_flag("--version", fmt::format("adm {}", mradm::version()));

    // ── render ────────────────────────────────────────────────────────────────
    std::string input;
    std::string output;
    std::string layout{"binaural"};
    std::string renderer{"auto"};
    bool verbose{false};

    auto* render_cmd = app.add_subcommand("render", "Render an ADM BWF file");
    render_cmd->add_option("-i,--input", input, "Input ADM BWF/WAV path")->required();
    render_cmd->add_option("-o,--output", output, "Output audio path");
    render_cmd->add_option("--output-layout", layout, "Output layout identifier");
    render_cmd->add_option("--renderer", renderer, "Renderer backend: auto, ear, saf, apple")
        ->check(CLI::IsMember({"auto", "ear", "saf", "apple"}));
    render_cmd->add_flag("-v,--verbose", verbose, "Enable verbose logs");

    // ── inspect ───────────────────────────────────────────────────────────────
    std::string inspect_input;
    auto* inspect_cmd = app.add_subcommand("inspect", "Print ADM scene metadata from a BWF file");
    inspect_cmd->add_option("file", inspect_input, "ADM BWF/WAV input path")->required();

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
        mradm::RenderRequest request;
        request.input_path = input;
        if (!output.empty()) {
            request.output_path = output;
        }
        request.options.output_layout = layout;
        request.options.renderer = parse_renderer(renderer);

        mradm::RenderService service;
        ConsoleProgressSink progress;
        SpdlogSink logs;
        mradm::RenderResult result = service.render(request, progress, logs);
        if (!result.success()) {
            spdlog::error(result.error.message);
            return EXIT_FAILURE;
        }
        if (result.output_path.has_value()) {
            spdlog::info("wrote {}", result.output_path->string());
        }
    }

    if (*inspect_cmd) {
        auto result = mradm::io::import_scene(inspect_input);
        if (!result.has_value()) {
            spdlog::error("{}", result.error().message);
            return EXIT_FAILURE;
        }
        print_scene(inspect_input, result.value());
    }

    if (*backends_cmd) {
        print_capabilities(mradm::ear_capabilities());
    }

    return EXIT_SUCCESS;
}
