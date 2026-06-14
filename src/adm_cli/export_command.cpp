#include <cstdlib>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "adm/io.h"
#include "adm/scene.h"
#include "adm/semantic_policy.h"

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

int run_export(const ExportCliOptions& opts) {
    auto imported = mradm::io::import_scene(opts.input);
    if (!imported.has_value()) {
        spdlog::error("{}", imported.error().message);
        return EXIT_FAILURE;
    }
    const mradm::AdmScene original = imported.value();
    mradm::AdmScene effective = original;

    if (!opts.semantic_policy_path.empty()) {
        auto policy = mradm::load_semantic_policy_file(opts.semantic_policy_path);
        if (!policy.has_value()) {
            spdlog::error("{}", policy.error().message);
            return EXIT_FAILURE;
        }
        std::vector<std::string> warnings;
        auto applied = mradm::apply_semantic_policy(effective, policy.value(), effective.info.sample_rate, &warnings);
        if (!applied.has_value()) {
            spdlog::error("{}", applied.error().message);
            return EXIT_FAILURE;
        }
        for (const auto& warning : warnings) {
            spdlog::warn("{}", warning);
        }
    }

    auto written = mradm::io::write_scene(opts.input, original, effective, opts.output);
    if (!written.has_value()) {
        spdlog::error("{}", written.error().message);
        return EXIT_FAILURE;
    }
    spdlog::info("wrote {}", opts.output);
    return EXIT_SUCCESS;
}
