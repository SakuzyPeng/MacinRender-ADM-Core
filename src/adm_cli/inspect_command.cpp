#include <string>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/io.h"
#include "adm/render.h"

#include "commands.h"

CLI::App* add_inspect_command(CLI::App& app, InspectCliOptions& opts) {
    auto* inspect_cmd = app.add_subcommand("inspect", "Print ADM or auto-detected channel-bed scene metadata");
    inspect_cmd->add_option("file", opts.input, "ADM BWF or channel-based WAVE input path")->required();
    inspect_cmd->add_flag("--xml", opts.xml, "Dump raw AXML chunk instead of parsed summary");
    inspect_cmd->add_option("--write-semantic-policy-template",
                            opts.semantic_policy_template_path,
                            "Write editable ADM semantic policy JSON template for this input");
    return inspect_cmd;
}

int run_inspect(const InspectCliOptions& opts) {
    if (opts.xml && !opts.semantic_policy_template_path.empty()) {
        spdlog::error("--xml cannot be combined with --write-semantic-policy-template");
        return EXIT_FAILURE;
    }
    if (opts.xml) {
        auto result = mradm::io::get_axml(opts.input);
        if (!result.has_value()) {
            spdlog::error("{}", result.error().message);
            return EXIT_FAILURE;
        }
        fmt::print("{}", result.value());
    } else if (!opts.semantic_policy_template_path.empty()) {
        const mradm::RenderService service;
        auto write_result = service.write_policy_template(opts.input, opts.semantic_policy_template_path, {});
        if (!write_result.has_value()) {
            spdlog::error("{}", write_result.error().message);
            return EXIT_FAILURE;
        }
        spdlog::info("wrote {}", opts.semantic_policy_template_path);
    } else {
        // The summary is Rust (mradm-engine `scene_json`, PCM consistency 5.6 C).
        const mradm::RenderService service;
        auto result = service.inspect_text(opts.input);
        if (!result.has_value()) {
            spdlog::error("{}", result.error().message);
            return EXIT_FAILURE;
        }
        fmt::print("{}", result.value());
    }
    return EXIT_SUCCESS;
}
