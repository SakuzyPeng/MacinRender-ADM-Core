#include <cstdlib>
#include <string>

#include <fmt/format.h>
#include <fmt/printf.h>

#include "adm/io.h"
#include "adm/render.h"

#include "commands.h"

CLI::App* add_input_layouts_command(CLI::App& app, InputLayoutCliOptions& opts) {
    auto* cmd = app.add_subcommand("input-layouts", "Show ordinary multichannel input layouts and label geometry");
    cmd->add_option("--format", opts.format, "Output format: text or json")->check(CLI::IsMember({"text", "json"}));
    cmd->add_option("--layout", opts.layout, "Optional preset filter, e.g. 5.1, 7.1.4, 22.2");
    return cmd;
}

int run_input_layouts(const InputLayoutCliOptions& opts) {
    if (opts.format == "json") {
        mradm::RenderService service;
        fmt::print("{}\n", service.input_layouts_json());
        return EXIT_SUCCESS;
    }

    fmt::print("Coordinate convention: azimuth +left / -right (0 front); elevation +up.\n");
    fmt::print("Aliases: L/FL=M+030 (+30°,0°), R/FR=M-030 (-30°,0°), C/FC=M+000, LFE=LFE1.\n");
    fmt::print("Custom mappings: 1-64 unique labels; U+110/U-110 require @30 or @45.\n\n");

    bool found = false;
    for (const auto& layout : mradm::io::input_layouts()) {
        if (!opts.layout.empty() && opts.layout != layout.id && opts.layout != layout.internal_id) {
            continue;
        }
        found = true;
        fmt::print("{} ({} channels, internal {})", layout.id, layout.channels.size(), layout.internal_id);
        if (layout.wave_channel_mask != 0U) {
            fmt::print("; WAVE mask 0x{:X}", layout.wave_channel_mask);
        }
        fmt::print("\n");
        fmt::print("     file order:");
        for (const auto& channel : layout.channels) {
            fmt::print(" {}", channel.token);
        }
        fmt::print("\n");
        for (std::size_t index = 0; index < layout.channels.size(); ++index) {
            const auto& channel = layout.channels[index];
            if (channel.is_lfe) {
                fmt::print("  {:>2}: {:<8} LFE (no geometric position)\n", index + 1U, channel.speaker_label);
            } else {
                fmt::print("  {:>2}: {:<8} az={:>6.1f}° el={:>5.1f}°\n",
                           index + 1U,
                           channel.speaker_label,
                           channel.azimuth,
                           channel.elevation);
            }
        }
        fmt::print("\n");
    }
    if (!found) {
        fmt::print(stderr, "unknown input layout '{}'; use mradm input-layouts without --layout\n", opts.layout);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
