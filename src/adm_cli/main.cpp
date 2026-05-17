#include <cstdlib>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/render.h"
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
    if (value == "auto") {
        return mradm::RendererSelection::automatic;
    }
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

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"MacinRender ADM Core command-line interface"};
    app.set_version_flag("--version", fmt::format("adm {}", mradm::version()));

    std::string input;
    std::string output;
    std::string layout{"binaural"};
    std::string renderer{"auto"};
    bool verbose{false};

    auto* render = app.add_subcommand("render", "Render an ADM BWF file");
    render->add_option("-i,--input", input, "Input ADM BWF/WAV path")->required();
    render->add_option("-o,--output", output, "Output audio path");
    render->add_option("--output-layout", layout, "Output layout identifier");
    render->add_option("--renderer", renderer, "Renderer backend: auto, ear, saf, apple")
        ->check(CLI::IsMember({"auto", "ear", "saf", "apple"}));
    render->add_flag("-v,--verbose", verbose, "Enable verbose logs");

    app.require_subcommand(1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    spdlog::set_pattern("%^[%l]%$ %v");
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);

    if (*render) {
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
        spdlog::info("render completed");
    }

    return EXIT_SUCCESS;
}
