#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

#include "commands.h"

namespace {

std::string lower_copy(std::string value) {
    std::ranges::transform(
        value, value.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return value;
}

std::string normalize_layout_query(const std::string& layout) {
    auto key = lower_copy(layout);
    if (key.empty()) {
        return {};
    }
    if (key == "binaural") {
        return "binaural";
    }
    if (key == "5.1" || key == "0+5+0") {
        return "5.1";
    }
    if (key == "5.1.2" || key == "2+5+0") {
        return "5.1.2";
    }
    if (key == "7.1" || key == "wav71" || key == "0+7+0") {
        return "7.1";
    }
    if (key == "5.1.4" || key == "4+5+0" || key == "atmos514") {
        return "5.1.4";
    }
    if (key == "7.1.4" || key == "4+7+0" || key == "atmos714") {
        return "7.1.4";
    }
    if (key == "9.1.6" || key == "atmos916") {
        return "9.1.6";
    }
    if (key == "22.2" || key == "9+10+3" || key == "cicp13") {
        return "22.2";
    }
    if (key == "hoa3") {
        return "hoa3";
    }
    return key;
}

std::string normalize_layout_format(const std::string& format) {
    auto key = lower_copy(format);
    if (key == "wave") {
        return "wav";
    }
    if (key == "m4a" || key == "mp4") {
        return "apac";
    }
    if (key == "iamf") {
        return "iamf";
    }
    return key;
}

bool renderer_supports_row(const mradm::OutputLayoutRow& row, std::string_view renderer) {
    return std::ranges::find(row.supported_by, renderer) != row.supported_by.end();
}

// Backend display name for the optional "Renderer:" header line. Only the name
// is needed here; the channel-order data comes from RenderService.
std::string backend_name_for(std::string_view renderer) {
    if (renderer == "ear") {
        return mradm::ear_capabilities().backend_name;
    }
    if (renderer == "saf") {
        return mradm::vbap_capabilities().backend_name;
    }
    if (renderer == "hoa") {
        return mradm::hoa_capabilities().backend_name;
    }
    if (renderer == "binaural") {
        return mradm::binaural_capabilities().backend_name;
    }
    return {};
}

int print_layouts(std::string format, const std::string& layout_filter, std::string renderer_filter) {
    format = normalize_layout_format(format);
    const auto layout = normalize_layout_query(layout_filter);
    renderer_filter = lower_copy(renderer_filter);
    const bool has_renderer = !renderer_filter.empty();
    const auto heading = format == "apac" ? std::string_view{"apac/m4a"} : std::string_view{format};

    const mradm::RenderService service;
    const auto rows = service.output_layouts();

    fmt::print("Format: {}\n", heading);
    if (has_renderer) {
        fmt::print("Renderer: {}\n", backend_name_for(renderer_filter));
    }
    fmt::print("{:<10} {:<8} {:<46} {}\n", "Layout", "Channels", "Container mapping", "Final channel order");
    fmt::print("{:-<10} {:-<8} {:-<46} {:-<19}\n", "", "", "", "");

    bool any{false};
    for (const auto& row : rows) {
        if (row.format != format) {
            continue;
        }
        if (!layout.empty() && row.layout != layout) {
            continue;
        }
        if (has_renderer && !renderer_supports_row(row, renderer_filter)) {
            continue;
        }
        any = true;
        fmt::print("{:<10} {:<8} {:<46} {}\n", row.layout, row.channels, row.container, row.order);
        if (!row.note.empty()) {
            fmt::print("{:<10} {:<8} {:<46} note: {}\n", "", "", "", row.note);
        }
    }

    if (!any) {
        if (layout.empty()) {
            if (has_renderer) {
                fmt::print(
                    stderr, "No layout is supported for format '{}' with renderer '{}'.\n", format, renderer_filter);
            } else {
                fmt::print(stderr, "No channel-order table is available for format '{}'.\n", format);
            }
        } else if (has_renderer) {
            fmt::print(stderr,
                       "Layout '{}' is not supported for format '{}' with renderer '{}'.\n",
                       layout,
                       format,
                       renderer_filter);
        } else {
            fmt::print(stderr, "Layout '{}' is not supported for format '{}'.\n", layout, format);
        }
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

CLI::App* add_layouts_command(CLI::App& app, LayoutCliOptions& opts) {
    auto* layouts_cmd =
        app.add_subcommand("layouts", "Show final channel order for an output format, optionally filtered by renderer");
    layouts_cmd->add_option("--format", opts.format, "Output format: wav, caf, flac, apac/m4a/mp4, iamf")
        ->required()
        ->check(CLI::IsMember({"wav", "wave", "caf", "flac", "apac", "m4a", "mp4", "iamf"}));
    layouts_cmd->add_option("--layout", opts.layout, "Optional layout filter, e.g. 7.1, 9.1.6, 22.2, binaural");
    layouts_cmd->add_option("--renderer", opts.renderer, "Optional renderer filter: ear, saf, hoa, binaural")
        ->check(CLI::IsMember({"ear", "saf", "hoa", "binaural"}));
    return layouts_cmd;
}

int run_layouts(const LayoutCliOptions& opts) {
    return print_layouts(opts.format, opts.layout, opts.renderer);
}
