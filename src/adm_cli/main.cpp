#include <cstdlib>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/version.h"

#include "commands.h"

int main(int argc, char** argv) {
    CLI::App app{"MacinRender ADM Core command-line interface"};
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): CLI11 owns the version callback.
    app.set_version_flag("--version", fmt::format("mradm {}", mradm::version()));

    RenderCliOptions render_opts;
    bool verbose{false};
    auto* render_cmd = add_render_command(app, render_opts);
    render_cmd->add_flag("-v,--verbose", verbose, "Enable verbose logs");

    InspectCliOptions inspect_opts;
    auto* inspect_cmd = add_inspect_command(app, inspect_opts);

    auto* backends_cmd = add_backends_command(app);

    LayoutCliOptions layouts_opts;
    auto* layouts_cmd = add_layouts_command(app, layouts_opts);

    auto* formats_cmd = add_formats_command(app);

    ExportCliOptions export_opts;
    auto* export_cmd = add_export_command(app, export_opts);

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
        return run_inspect(inspect_opts);
    }
    if (*backends_cmd) {
        run_backends();
        return EXIT_SUCCESS;
    }
    if (*layouts_cmd) {
        return run_layouts(layouts_opts);
    }
    if (*formats_cmd) {
        run_formats();
        return EXIT_SUCCESS;
    }
    if (*export_cmd) {
        return run_export(export_opts);
    }
    return EXIT_SUCCESS;
}
