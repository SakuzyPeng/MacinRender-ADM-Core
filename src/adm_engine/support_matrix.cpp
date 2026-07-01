#include "support_matrix.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/capability.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

#include "format_table.h"
#include "layout_table.h"

namespace mradm::engine {

namespace {

using nlohmann::json;

struct BackendInfo { // NOLINT(cppcoreguidelines-pro-type-member-init)
    std::string renderer;
    CapabilityReport capabilities;
};

struct LayoutSummary { // NOLINT(cppcoreguidelines-pro-type-member-init)
    std::string layout;
    std::string layout_id;
    uint32_t channels{};
    bool is_3d{};
};

struct OutputTarget { // NOLINT(cppcoreguidelines-pro-type-member-init)
    std::string target;
    std::string format;
    std::string container;
    std::string encoding;
    std::vector<std::string> extensions;
    std::string layout_format;
    std::string required_option_name;
    std::string required_option_value;
};

std::vector<BackendInfo> build_backends() {
    std::vector<BackendInfo> backends;
    backends.push_back({"ear", ear_capabilities()});
    backends.push_back({"saf", vbap_capabilities()});
    backends.push_back({"hoa", hoa_capabilities()});
    backends.push_back({"saf-binaural", binaural_capabilities()});
#ifdef __APPLE__
    backends.push_back({"apple", apple_capabilities()});
#endif
    return backends;
}

[[nodiscard]] std::string capability_layout_id(std::string_view layout) {
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

[[nodiscard]] bool display_layout_has_height(std::string_view layout) {
    return layout == "5.1.2" || layout == "5.1.4" || layout == "7.1.4" || layout == "9.1.4" || layout == "9.1.6" ||
           layout == "22.2" || layout == "hoa3";
}

[[nodiscard]] std::optional<CapabilityReport::Layout> find_backend_layout(const CapabilityReport& caps,
                                                                          std::string_view display_layout) {
    const auto id = capability_layout_id(display_layout);
    const auto it = std::ranges::find_if(caps.supported_layouts, [&](const CapabilityReport::Layout& layout) {
        return layout.id == id || (display_layout == "binaural" && layout.is_binaural);
    });
    if (it == caps.supported_layouts.end()) {
        return std::nullopt;
    }
    return *it;
}

[[nodiscard]] bool
has_layout_row(const std::vector<OutputLayoutRow>& rows, std::string_view format, std::string_view layout) {
    return std::ranges::any_of(
        rows, [&](const OutputLayoutRow& row) { return row.format == format && row.layout == layout; });
}

[[nodiscard]] const OutputFormatInfo* find_format(const OutputFormats& formats, std::string_view format) {
    const auto it =
        std::ranges::find_if(formats.formats, [&](const OutputFormatInfo& info) { return info.format == format; });
    if (it == formats.formats.end()) {
        return nullptr;
    }
    return &*it;
}

[[nodiscard]] std::string quoted_reason(std::string_view prefix, std::string_view value, std::string_view suffix) {
    std::string out;
    out.reserve(prefix.size() + value.size() + suffix.size() + 2U);
    out.append(prefix);
    out.push_back('\'');
    out.append(value);
    out.push_back('\'');
    out.append(suffix);
    return out;
}

[[nodiscard]] std::string quoted_pair_reason(std::string_view prefix,
                                             std::string_view first,
                                             std::string_view infix,
                                             std::string_view second,
                                             std::string_view suffix = {}) {
    std::string out;
    out.reserve(prefix.size() + first.size() + infix.size() + second.size() + suffix.size() + 4U);
    out.append(prefix);
    out.push_back('\'');
    out.append(first);
    out.push_back('\'');
    out.append(infix);
    out.push_back('\'');
    out.append(second);
    out.push_back('\'');
    out.append(suffix);
    return out;
}

[[nodiscard]] std::vector<LayoutSummary> build_layouts(const std::vector<OutputLayoutRow>& rows) {
    std::vector<LayoutSummary> layouts;
    for (const auto& row : rows) {
        if (std::ranges::any_of(layouts,
                                [&](const LayoutSummary& existing) { return existing.layout == row.layout; })) {
            continue;
        }
        layouts.push_back({.layout = row.layout,
                           .layout_id = capability_layout_id(row.layout),
                           .channels = row.channels,
                           .is_3d = display_layout_has_height(row.layout)});
    }
    return layouts;
}

[[nodiscard]] std::vector<OutputTarget> build_targets() {
    return {
        {.target = "wav",
         .format = "wav",
         .container = "wav",
         .encoding = "pcm",
         .extensions = {".wav"},
         .layout_format = "wav"},
        {.target = "caf",
         .format = "caf",
         .container = "caf",
         .encoding = "pcm",
         .extensions = {".caf"},
         .layout_format = "caf"},
        {.target = "flac",
         .format = "flac",
         .container = "flac",
         .encoding = "flac",
         .extensions = {".flac"},
         .layout_format = "flac"},
        {.target = "opus_mka", .format = "opus_mka", .container = "mka", .encoding = "opus", .extensions = {".mka"}},
        {.target = "apac_mpeg4",
         .format = "apac",
         .container = "mpeg4",
         .encoding = "apac",
         .extensions = {".m4a", ".mp4"},
         .layout_format = "apac"},
        {.target = "apac_caf",
         .format = "apac",
         .container = "caf",
         .encoding = "apac",
         .extensions = {".caf"},
         .layout_format = "apac",
         .required_option_name = "apac_container",
         .required_option_value = "caf"},
        {.target = "iamf",
         .format = "iamf",
         .container = "iamf",
         .encoding = "iamf_opus",
         .extensions = {".iamf"},
         .layout_format = "iamf"},
        {.target = "iamf_mp4",
         .format = "iamf_mp4",
         .container = "mp4",
         .encoding = "iamf_opus",
         .extensions = {".mp4"},
         .layout_format = "iamf",
         .required_option_name = "iamf_container",
         .required_option_value = "mp4"},
    };
}

[[nodiscard]] std::string support_reason(const BackendInfo& backend,
                                         const LayoutSummary& layout,
                                         const OutputTarget& target,
                                         const OutputFormatInfo& format,
                                         const std::vector<OutputLayoutRow>& layout_rows) {
    if (!find_backend_layout(backend.capabilities, layout.layout).has_value()) {
        return quoted_pair_reason("renderer ", backend.renderer, " does not support layout ", layout.layout);
    }
    if (!format.available) {
        return format.available_reason.empty() ? quoted_reason("format ", target.format, " is unavailable")
                                               : format.available_reason;
    }
    if (!target.layout_format.empty() && !has_layout_row(layout_rows, target.layout_format, layout.layout)) {
        return quoted_pair_reason("target ", target.target, " does not define layout ", layout.layout);
    }
    if (format.max_channels > 0U && layout.channels > format.max_channels) {
        std::string out;
        out.reserve(target.target.size() + layout.layout.size() + 80U);
        out.append("target '");
        out.append(target.target);
        out.append("' supports at most ");
        out.append(std::to_string(format.max_channels));
        out.append(" channels; layout '");
        out.append(layout.layout);
        out.append("' has ");
        out.append(std::to_string(layout.channels));
        out.append(" channels");
        return out;
    }
    if (!format.supports_height && layout.is_3d) {
        std::string out;
        out.reserve(target.target.size() + 45U);
        out.append("target '");
        out.append(target.target);
        out.append("' does not support height/3D layouts");
        return out;
    }
    return {};
}

json features_to_json(const OutputFormatFeatures& features) {
    json j = json::object();
    j["apac"] = features.apac;
    j["iamf"] = features.iamf;
    j["iamf_mp4_packager"] = features.iamf_mp4_packager;
    j["sofa"] = features.sofa;
    return j;
}

json backend_to_json(const BackendInfo& backend) {
    json j = json::object();
    j["renderer"] = backend.renderer;
    j["backend_name"] = backend.capabilities.backend_name;
    j["backend_version"] = backend.capabilities.backend_version;
    return j;
}

json layout_to_json(const LayoutSummary& layout) {
    json j = json::object();
    j["layout"] = layout.layout;
    j["layout_id"] = layout.layout_id;
    j["channels"] = layout.channels;
    j["is_3d"] = layout.is_3d;
    return j;
}

json target_to_json(const OutputTarget& target, const OutputFormatInfo& format) {
    json j = json::object();
    j["target"] = target.target;
    j["format"] = target.format;
    j["container"] = target.container;
    j["encoding"] = target.encoding;
    j["extensions"] = target.extensions;
    j["available"] = format.available;
    if (!format.available && !format.available_reason.empty()) {
        j["available_reason"] = format.available_reason;
    }
    j["lossy"] = format.lossy;
    j["max_channels"] = format.max_channels;
    j["fixed_sample_rate"] = format.fixed_sample_rate;
    j["supports_height"] = format.supports_height;
    if (!target.required_option_name.empty()) {
        j["required_option"] = {{"name", target.required_option_name}, {"value", target.required_option_value}};
    }
    return j;
}

json entry_to_json(const BackendInfo& backend,
                   const LayoutSummary& layout,
                   const OutputTarget& target,
                   const OutputFormatInfo& format,
                   const std::vector<OutputLayoutRow>& layout_rows) {
    json j = json::object();
    j["renderer"] = backend.renderer;
    j["layout"] = layout.layout;
    j["layout_id"] = layout.layout_id;
    j["channels"] = layout.channels;
    j["is_3d"] = layout.is_3d;
    j["target"] = target.target;
    j["format"] = target.format;
    j["container"] = target.container;
    j["encoding"] = target.encoding;
    const std::string reason = support_reason(backend, layout, target, format, layout_rows);
    j["supported"] = reason.empty();
    if (!reason.empty()) {
        j["reason"] = reason;
    }
    return j;
}

} // namespace

std::string render_support_matrix_to_json() {
    const auto backends = build_backends();
    const auto layout_rows = build_output_layouts();
    const auto layouts = build_layouts(layout_rows);
    const auto targets = build_targets();
    const auto formats = build_output_formats();

    json root = json::object();
    root["schema"] = "mradm.render-support-matrix";
    root["schema_version"] = 1;
    root["features"] = features_to_json(formats.features);

    json backend_json = json::array();
    std::ranges::transform(backends, std::back_inserter(backend_json), backend_to_json);
    root["backends"] = std::move(backend_json);

    json layout_json = json::array();
    std::ranges::transform(layouts, std::back_inserter(layout_json), layout_to_json);
    root["layouts"] = std::move(layout_json);

    json target_json = json::array();
    for (const auto& target : targets) {
        if (const auto* format = find_format(formats, target.format); format != nullptr) {
            target_json.push_back(target_to_json(target, *format));
        }
    }
    root["targets"] = std::move(target_json);

    json entries = json::array();
    for (const auto& backend : backends) {
        for (const auto& layout : layouts) {
            for (const auto& target : targets) {
                const auto* format = find_format(formats, target.format);
                if (format == nullptr) {
                    continue;
                }
                entries.push_back(entry_to_json(backend, layout, target, *format, layout_rows));
            }
        }
    }
    root["entries"] = std::move(entries);

    return root.dump(2);
}

} // namespace mradm::engine
