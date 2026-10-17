#include <cstdlib>
#include <string_view>

#include <spdlog/spdlog.h>

#include "adm/logging.h"
#include "adm/render.h"

#include "commands.h"

CLI::App* add_export_command(CLI::App& app, ExportCliOptions& opts) {
    auto* export_cmd =
        app.add_subcommand("export", "Write a new ADM BWF with semantic-policy overrides applied (source PCM reused)");
    export_cmd->add_option("-i,--input", opts.input, "ADM BWF/WAV input path")->required();
    export_cmd->add_option("-o,--output", opts.output, "Output ADM BWF path")->required();
    export_cmd->add_option("--semantic-policy",
                           opts.semantic_policy_path,
                           "Semantic policy JSON to apply before writing (optional; omit for a plain round-trip)");
    return export_cmd;
}

namespace {
// Export output stays as before the policy moved to Rust: policy warnings as plain warnings and
// no importer or engine diagnostics.
class ExportLogs final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel /*level*/, std::string_view module, std::string_view message) override {
        if (module == "semantic-policy") {
            spdlog::warn("{}", message);
        }
    }
};
} // namespace

int run_export(const ExportCliOptions& opts) {
    mradm::RenderOptions options;
    if (!opts.semantic_policy_path.empty()) {
        options.semantic_policy_path = opts.semantic_policy_path;
    }
    ExportLogs logs;
    const mradm::RenderService service;
    auto written = service.export_file(opts.input, opts.output, options, logs);
    if (!written.has_value()) {
        spdlog::error("{}", written.error().message);
        return EXIT_FAILURE;
    }
    spdlog::info("wrote {}", opts.output);
    return EXIT_SUCCESS;
}
