#include "catalog.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine_test_ffi.h"
#include "platform_island.h"

namespace mradm::engine {

std::string document(Document kind) {
    MradmEngineText* raw = nullptr;
    MradmAdmText json{};
    if (mradm_engine_document(static_cast<uint32_t>(kind), mradm_platform_islands(), &raw, &json) != 0) {
        throw std::runtime_error("discovery document is unavailable");
    }
    const std::unique_ptr<MradmEngineText, decltype(&mradm_engine_text_destroy)> owned(raw, &mradm_engine_text_destroy);
    return {reinterpret_cast<const char*>(json.data), json.len};
}

std::vector<OutputLayoutRow> build_output_layouts() {
    const auto root = nlohmann::json::parse(document(Document::layouts));
    std::vector<OutputLayoutRow> rows(root.at("layouts").size());
    std::ranges::transform(root.at("layouts"), rows.begin(), [](const nlohmann::json& j) -> OutputLayoutRow {
        return {j.at("format").get<std::string>(),
                j.at("layout").get<std::string>(),
                j.at("channels").get<uint32_t>(),
                j.at("container").get<std::string>(),
                j.at("order").get<std::string>(),
                j.value("note", std::string{}),
                j.at("supported_by").get<std::vector<std::string>>()};
    });
    return rows;
}

OutputFormats build_output_formats() {
    const auto root = nlohmann::json::parse(document(Document::output_formats));
    OutputFormats out;
    const auto& f = root.at("features");
    out.features = {.apac = f.at("apac").get<bool>(),
                    .iamf = f.at("iamf").get<bool>(),
                    .iamf_mp4_packager = f.at("iamf_mp4_packager").get<bool>(),
                    .sofa = f.at("sofa").get<bool>(),
                    .hptf = f.at("hptf").get<bool>()};
    for (const auto& j : root.at("formats")) {
        OutputFormatInfo info;
        info.format = j.at("format").get<std::string>();
        info.extensions = j.at("extensions").get<std::vector<std::string>>();
        info.available = j.at("available").get<bool>();
        info.available_reason = j.value("available_reason", std::string{});
        info.lossy = j.at("lossy").get<bool>();
        info.max_channels = j.at("max_channels").get<uint32_t>();
        info.fixed_sample_rate = j.at("fixed_sample_rate").get<uint32_t>();
        info.supports_height = j.at("supports_height").get<bool>();
        info.bit_depths = j.value("bit_depths", std::vector<std::string>{});
        info.note = j.value("note", std::string{});
        for (const auto* key : {"bitrate_kbps_per_ch", "bitrate_kbps_total"}) {
            if (j.contains(key)) {
                info.has_bitrate = true;
                info.bitrate_per_channel = std::string_view{key} == "bitrate_kbps_per_ch";
                info.bitrate_min_kbps = j.at(key).at("min").get<uint32_t>();
                info.bitrate_max_kbps = j.at(key).at("max").get<uint32_t>();
            }
        }
        out.formats.push_back(std::move(info));
    }
    return out;
}

} // namespace mradm::engine
