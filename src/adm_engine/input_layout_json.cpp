#include "input_layout_json.h"

#include <algorithm>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include "adm/io.h"

namespace mradm::engine {

std::string input_layouts_to_json() {
    using nlohmann::json;
    json root = json::object();
    root["schema"] = "mradm.input-layouts";
    root["schema_version"] = 1;
    root["max_channels"] = 64;
    root["containers"] = {"WAVE", "RF64", "BW64"};
    root["sample_encodings"] = {"pcm16", "pcm24", "pcm32", "float32"};
    root["auto_detection"] = {
        {"axml_present", "strict ADM import; invalid ADM never falls back"},
        {"axml_absent", "recognised WAVEFORMATEXTENSIBLE channel mask"},
        {"unrecognised_or_missing_mask", "explicit input layout or channel labels required"},
    };
    root["custom_constraints"] = {
        {"label_count_must_match_file_channels", true},
        {"empty_labels_allowed", false},
        {"duplicate_canonical_labels_allowed", false},
        {"arbitrary_coordinates_allowed", false},
        {"ambiguous_labels", {{"U+110", {"U+110@30", "U+110@45"}}, {"U-110", {"U-110@30", "U-110@45"}}}},
    };
    root["two_channel_output_semantic"] = "binaural";
    root["coordinate_convention"] = {
        {"azimuth", "degrees; positive left, negative right, 0 front"},
        {"elevation", "degrees; positive up, 0 horizontal"},
        {"distance", "normalised; 1.0 for channel-bed speakers"},
    };
    root["aliases"] = {
        {"L", "M+030"},
        {"FL", "M+030"},
        {"R", "M-030"},
        {"FR", "M-030"},
        {"C", "M+000"},
        {"FC", "M+000"},
        {"LFE", "LFE1"},
    };

    json layouts = json::array();
    for (const auto& layout : io::input_layouts()) {
        json item = json::object();
        item["id"] = layout.id;
        item["internal_id"] = layout.internal_id;
        item["display_name"] = layout.display_name;
        item["channel_count"] = layout.channels.size();
        if (layout.wave_channel_mask != 0U) {
            item["wave_channel_mask"] = layout.wave_channel_mask;
            json mask_order = json::array();
            std::ranges::transform(layout.wave_mask_channels, std::back_inserter(mask_order), [](const auto& channel) {
                return channel.token;
            });
            item["wave_mask_channel_order"] = std::move(mask_order);
        }
        json channels = json::array();
        std::ranges::transform(layout.channels, std::back_inserter(channels), [](const auto& channel) {
            return json{
                {"token", channel.token},
                {"speaker_label", channel.speaker_label},
                {"azimuth", channel.azimuth},
                {"elevation", channel.elevation},
                {"is_lfe", channel.is_lfe},
            };
        });
        item["channels"] = std::move(channels);
        layouts.push_back(std::move(item));
    }
    root["layouts"] = std::move(layouts);

    json catalog = json::array();
    std::ranges::transform(io::input_channel_catalog(), std::back_inserter(catalog), [](const auto& channel) {
        return json{
            {"token", channel.token},
            {"speaker_label", channel.speaker_label},
            {"azimuth", channel.azimuth},
            {"elevation", channel.elevation},
            {"is_lfe", channel.is_lfe},
        };
    });
    root["custom_channel_catalog"] = std::move(catalog);
    return root.dump(2);
}

} // namespace mradm::engine
