#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

#include "commands.h"

namespace {

struct LayoutInfo {
    std::string_view format;
    std::string_view layout;
    uint32_t channels;
    std::string_view container;
    std::string_view order;
    std::string_view note;
};

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

const mradm::CapabilityReport* capabilities_for_renderer(std::string_view renderer) {
    static const auto ear = mradm::ear_capabilities();
    static const auto saf = mradm::vbap_capabilities();
    static const auto hoa = mradm::hoa_capabilities();
    static const auto binaural = mradm::binaural_capabilities();

    if (renderer == "ear") {
        return &ear;
    }
    if (renderer == "saf") {
        return &saf;
    }
    if (renderer == "hoa") {
        return &hoa;
    }
    if (renderer == "binaural") {
        return &binaural;
    }
    return nullptr;
}

std::string capability_layout_id(std::string_view layout) {
    if (layout == "5.1") {
        return "0+5+0";
    }
    if (layout == "5.1.2") {
        return "2+5+0";
    }
    if (layout == "7.1") {
        return "wav71";
    }
    if (layout == "5.1.4") {
        return "4+5+0";
    }
    if (layout == "9.1.4") {
        return "4+5+4";
    }
    if (layout == "7.1.4") {
        return "4+7+0";
    }
    if (layout == "22.2") {
        return "9+10+3";
    }
    return std::string{layout};
}

bool renderer_supports_layout(const mradm::CapabilityReport& caps, std::string_view layout) {
    const auto id = capability_layout_id(layout);
    return std::ranges::any_of(caps.supported_layouts, [&](const auto& supported) {
        return supported.id == id || (layout == "binaural" && supported.is_binaural);
    });
}

constexpr std::array<LayoutInfo, 34> k_layout_infos{{
    {"wav",
     "binaural",
     2,
     "plain WAV; metadata layout=binaural",
     "Binaural L, Binaural R",
     "No speaker-stereo ADM render path is exposed."},
    {"wav", "5.1", 6, "plain WAV sample order", "L R C LFE Ls Rs", ""},
    {"wav", "5.1.2", 8, "plain WAV sample order", "L R C LFE Ls Rs U+030 U-030", "Top channels use ADM labels."},
    {"wav", "7.1", 8, "plain WAV sample order", "L R C LFE Rls Rrs Ls Rs", "WAVE_7_1 / wav71 order."},
    {"wav",
     "5.1.4",
     10,
     "plain WAV sample order",
     "L R C LFE Ls Rs U+030 U-030 U+110 U-110",
     "Top channels use ADM labels."},
    {"wav",
     "7.1.4",
     12,
     "plain WAV sample order",
     "L R C LFE Ls Rs Rls Rrs U+045 U-045 U+135 U-135",
     "Top channels use ADM labels."},
    {"wav", "9.1.4", 14, "plain WAV sample order", "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltr Rtr", ""},
    {"wav", "9.1.6", 16, "plain WAV sample order", "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr", ""},
    {"wav",
     "22.2",
     24,
     "plain WAV sample order",
     "M+060 M-060 M+000 LFE1 M+135 M-135 M+030 M-030 M+180 LFE2 M+090 M-090 U+045 U-045 "
     "U+000 T+000 U+135 U-135 U+090 U-090 U+180 B+000 B+045 B-045",
     "BS.2051/libear order; LFE names differ from CoreAudio CICP_13 names."},
    {"wav", "hoa3", 16, "plain WAV sample order", "ACN 0..15, SN3D", "HOA encode output, not a speaker layout."},

    {"caf", "binaural", 2, "CoreAudio Binaural", "BinauralLeft BinauralRight", ""},
    {"caf", "5.1", 6, "CoreAudio MPEG_5_1_A / CICP_6", "L R C LFE Ls Rs", ""},
    {"caf", "7.1", 8, "CoreAudio WAVE_7_1", "L R C LFE Rls Rrs Ls Rs", "Same order as internal wav71."},
    {"caf", "hoa3", 16, "CoreAudio HOA_ACN_SN3D", "ACN 0..15, SN3D", "HOA encode output, not a speaker layout."},
    {"caf", "5.1.4", 10, "CoreAudio Atmos_5_1_4", "L R C LFE Ls Rs Vhl Vhr Ltr Rtr", ""},
    {"caf", "7.1.4", 12, "CoreAudio Atmos_7_1_4", "L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr", ""},
    {"caf", "9.1.6", 16, "CoreAudio Atmos_9_1_6", "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr", ""},
    {"caf",
     "22.2",
     24,
     "CoreAudio CICP_13",
     "Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb",
     "CoreAudio names the two LFE slots LFE2/LFE3."},

    {"flac",
     "binaural",
     2,
     "FLAC Vorbis Comment layout=binaural",
     "Binaural L, Binaural R",
     "Readers may display this as ordinary stereo."},
    {"flac", "5.1", 6, "FLAC + WAVEFORMATEXTENSIBLE mask 0x0000003F", "L R C LFE Ls Rs", ""},
    {"flac",
     "7.1",
     8,
     "FLAC + WAVEFORMATEXTENSIBLE mask 0x0000063F",
     "L R C LFE Rls Rrs Ls Rs",
     "WAVE_7_1 / wav71 order."},

    {"apac",
     "binaural",
     2,
     "APAC requests CoreAudio Binaural",
     "L R",
     "afinfo currently reports Stereo; metadata preserves layout=binaural."},
    {"apac",
     "7.1",
     8,
     "CoreAudio AudioUnit_7_1",
     "L R C LFE Ls Rs Rls Rrs",
     "Input wav71 is reordered before APAC encoding."},
    {"apac", "hoa3", 16, "CoreAudio HOA_ACN_SN3D", "ACN 0..15, SN3D", "HOA encode output, not a speaker layout."},
    {"apac", "5.1.4", 10, "CoreAudio Atmos_5_1_4", "L R C LFE Ls Rs Vhl Vhr Ltr Rtr", ""},
    {"apac", "7.1.4", 12, "CoreAudio Atmos_7_1_4", "L R C LFE Ls Rs Rls Rrs Vhl Vhr Ltr Rtr", ""},
    {"apac", "9.1.6", 16, "CoreAudio Atmos_9_1_6", "L R C LFE Ls Rs Rls Rrs Lw Rw Vhl Vhr Ltm Rtm Ltr Rtr", ""},
    {"apac",
     "22.2",
     24,
     "CoreAudio CICP_13",
     "Lw Rw C LFE2 Rls Rrs L R Cs LFE3 Lss Rss Vhl Vhr Vhc Ts Ltr Rtr Ltm Rtm Ctr Cb Lb Rb",
     "CoreAudio names the two LFE slots LFE2/LFE3."},

    {"iamf", "5.1", 6, "IAMF kLayout5_1_ch (Opus VBR)", "FL FR SiL SiR C LFE (encoding order)", ""},
    {"iamf", "5.1.2", 8, "IAMF kLayout5_1_2_ch (Opus VBR)", "FL FR SiL SiR TpFL TpFR C LFE (encoding order)", ""},
    {"iamf", "5.1.4", 10, "IAMF kLayout5_1_4_ch (Opus VBR)", "FL FR SiL SiR TpFL TpFR TpBL TpBR C LFE (enc order)", ""},
    {"iamf", "7.1", 8, "IAMF kLayout7_1_ch (Opus VBR)", "FL FR Ls Rs Rls Rrs C LFE (wav71 reordered)", ""},
    {"iamf", "7.1.4", 12, "IAMF kLayout7_1_4_ch (Opus VBR)", "FL FR Ls Rs Rls Rrs TpFL TpFR TpBL TpBR C LFE", ""},
    {"iamf",
     "9.1.6",
     16,
     "IAMF Expanded 9.1.6 (Opus VBR)",
     "FL FR Rls Rrs Lw Rw Ls Rs Vhl Vhr Ltr Rtr Ltm Rtm C LFE",
     ""},
}};

int print_layouts(std::string format, const std::string& layout_filter, std::string renderer_filter) {
    format = normalize_layout_format(format);
    const auto layout = normalize_layout_query(layout_filter);
    renderer_filter = lower_copy(renderer_filter);
    const auto* renderer_caps = capabilities_for_renderer(renderer_filter);
    const auto heading = format == "apac" ? std::string_view{"apac/m4a"} : std::string_view{format};

    fmt::print("Format: {}\n", heading);
    if (renderer_caps != nullptr) {
        fmt::print("Renderer: {}\n", renderer_caps->backend_name);
    }
    fmt::print("{:<10} {:<8} {:<46} {}\n", "Layout", "Channels", "Container mapping", "Final channel order");
    fmt::print("{:-<10} {:-<8} {:-<46} {:-<19}\n", "", "", "", "");

    bool any{false};
    for (const auto& row : k_layout_infos) {
        if (row.format != format) {
            continue;
        }
        if (!layout.empty() && row.layout != layout) {
            continue;
        }
        if (renderer_caps != nullptr && !renderer_supports_layout(*renderer_caps, row.layout)) {
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
            if (renderer_caps != nullptr) {
                fmt::print(
                    stderr, "No layout is supported for format '{}' with renderer '{}'.\n", format, renderer_filter);
            } else {
                fmt::print(stderr, "No channel-order table is available for format '{}'.\n", format);
            }
        } else if (renderer_caps != nullptr) {
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
