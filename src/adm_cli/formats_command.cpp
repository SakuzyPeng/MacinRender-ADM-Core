#include <string>
#include <vector>

#include <fmt/format.h>

#include "adm/render.h"

#include "commands.h"

namespace {

std::string join_space(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) {
            out += ' ';
        }
        out += item;
    }
    return out;
}

void print_formats(const mradm::OutputFormats& data) {
    const auto yn = [](bool b) { return b ? "yes" : "no"; };
    fmt::print("Build features: apac={}  iamf={}  iamf_mp4_packager={}  sofa={}\n\n",
               yn(data.features.apac),
               yn(data.features.iamf),
               yn(data.features.iamf_mp4_packager),
               yn(data.features.sofa));

    for (const auto& f : data.formats) {
        fmt::print("{:<10} {}\n", f.format, join_space(f.extensions));
        if (f.available) {
            fmt::print("  available\n");
        } else {
            fmt::print("  unavailable: {}\n", f.available_reason);
        }
        fmt::print("  {}  channels: {}  sample rate: {}  height: {}\n",
                   f.lossy ? "lossy" : "lossless",
                   f.max_channels == 0 ? std::string{"any"} : fmt::format("up to {}", f.max_channels),
                   f.fixed_sample_rate == 0 ? std::string{"any"} : fmt::format("{} Hz", f.fixed_sample_rate),
                   yn(f.supports_height));
        if (!f.bit_depths.empty()) {
            fmt::print("  bit depths: {}\n", join_space(f.bit_depths));
        }
        if (f.has_bitrate) {
            fmt::print("  bitrate ({}): {}-{} kbps (0 = auto)\n",
                       f.bitrate_per_channel ? "per channel" : "total",
                       f.bitrate_min_kbps,
                       f.bitrate_max_kbps);
        }
        if (!f.note.empty()) {
            fmt::print("  note: {}\n", f.note);
        }
        fmt::print("\n");
    }
}

} // namespace

CLI::App* add_formats_command(CLI::App& app) {
    return app.add_subcommand("formats", "List output container formats with availability and constraints");
}

void run_formats() {
    const mradm::RenderService service;
    print_formats(service.output_formats());
}
