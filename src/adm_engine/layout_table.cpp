#include "layout_table.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "adm/capability.h"
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

namespace mradm::engine {

namespace {

using nlohmann::json;

// Static channel-order reference. The single source of truth for both
// `mradm layouts` and adm_layouts_json. supported_by is computed (not stored)
// so it can never drift from the actual renderer capabilities.
struct LayoutInfo {
    std::string_view format;
    std::string_view layout;
    uint32_t channels;
    std::string_view container;
    std::string_view order;
    std::string_view note;
};

constexpr std::array<LayoutInfo, 33> k_layout_infos{{
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
}};

// Map a display layout name to the CapabilityReport::Layout id used internally.
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

bool renderer_supports_layout(const CapabilityReport& caps, std::string_view layout) {
    const auto id = capability_layout_id(layout);
    return std::ranges::any_of(caps.supported_layouts, [&](const auto& supported) {
        return supported.id == id || (layout == "binaural" && supported.is_binaural);
    });
}

json row_to_json(const OutputLayoutRow& row) {
    json j = json::object();
    j["format"] = row.format;
    j["layout"] = row.layout;
    j["channels"] = row.channels;
    j["container"] = row.container;
    j["order"] = row.order;
    if (!row.note.empty()) {
        j["note"] = row.note;
    }
    j["supported_by"] = row.supported_by;
    return j;
}

} // namespace

std::vector<OutputLayoutRow> build_output_layouts() {
    static const auto ear = ear_capabilities();
    static const auto saf = vbap_capabilities();
    static const auto hoa = hoa_capabilities();
    static const auto binaural = binaural_capabilities();
    const std::array<std::pair<const char*, const CapabilityReport*>, 4> backends{{
        {"ear", &ear},
        {"saf", &saf},
        {"hoa", &hoa},
        {"binaural", &binaural},
    }};

    std::vector<OutputLayoutRow> rows;
    rows.reserve(k_layout_infos.size());
    for (const auto& info : k_layout_infos) {
        OutputLayoutRow row;
        row.format = info.format;
        row.layout = info.layout;
        row.channels = info.channels;
        row.container = info.container;
        row.order = info.order;
        row.note = info.note;
        for (const auto& [name, caps] : backends) {
            if (renderer_supports_layout(*caps, info.layout)) {
                row.supported_by.emplace_back(name);
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string layouts_to_json() {
    const auto rows = build_output_layouts();

    json root = json::object();
    root["schema"] = "mradm.layouts";
    root["schema_version"] = 1;

    json layouts = json::array();
    std::ranges::transform(rows, std::back_inserter(layouts), row_to_json);
    root["layouts"] = std::move(layouts);

    return root.dump(2);
}

} // namespace mradm::engine
