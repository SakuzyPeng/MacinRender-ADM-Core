#include "format_table.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/render_binaural.h"

namespace mradm::engine {

namespace {

using nlohmann::json;

// Common static description of an output container. Availability and any
// bitrate/bit-depth detail are attached per-format in output_formats_to_json().
struct FormatDesc {
    std::string_view format;
    std::vector<std::string_view> extensions;
    bool lossy;
    uint32_t max_channels;      // 0 = unlimited
    uint32_t fixed_sample_rate; // 0 = any
    bool supports_height;
    std::string_view note;
};

json base_format_json(const FormatDesc& d, bool available, std::string_view unavailable_reason) {
    json j = json::object();
    j["format"] = d.format;
    j["extensions"] = d.extensions;
    j["available"] = available;
    if (!available && !unavailable_reason.empty()) {
        j["available_reason"] = unavailable_reason;
    }
    j["lossy"] = d.lossy;
    j["max_channels"] = d.max_channels;
    j["fixed_sample_rate"] = d.fixed_sample_rate;
    j["supports_height"] = d.supports_height;
    if (!d.note.empty()) {
        j["note"] = d.note;
    }
    return j;
}

// Bitrate descriptor: {"min": .., "max": .., "auto": 0}. auto is always 0 (the
// API's "use the layout/encoder default" sentinel).
json bitrate_range(uint32_t min_kbps, uint32_t max_kbps) {
    json j = json::object();
    j["min"] = min_kbps;
    j["max"] = max_kbps;
    j["auto"] = 0;
    return j;
}

} // namespace

std::string output_formats_to_json() {
    const bool apac = audio::apac_encoding_available();
    const bool iamf = audio::iamf_encoding_available();
    const bool iamf_mp4 = iamf && audio::iamf_mp4_packager_available();
    const bool sofa = binaural_sofa_supported();

    json root = json::object();
    root["schema"] = "mradm.output-formats";
    root["schema_version"] = 1;

    json features = json::object();
    features["apac"] = apac;
    features["iamf"] = iamf;
    features["iamf_mp4_packager"] = iamf_mp4;
    features["sofa"] = sofa;
    root["features"] = std::move(features);

    json formats = json::array();

    // wav — always available; bit depth selectable.
    {
        const FormatDesc d{"wav", {".wav"}, false, 0, 0, true, "Uncompressed PCM; not a preferred delivery format."};
        json j = base_format_json(d, true, {});
        j["bit_depths"] = json::array({"f32", "i24", "i16"});
        formats.push_back(std::move(j));
    }
    // caf — always available; float32, CoreAudio channel layouts.
    {
        const FormatDesc d{
            "caf", {".caf"}, false, 0, 0, true, "Float32 CoreAudio container; richer metadata on macOS."};
        json j = base_format_json(d, true, {});
        j["bit_depths"] = json::array({"f32"});
        formats.push_back(std::move(j));
    }
    // flac — always available; 1-8 channels, no height, fixed 24-bit.
    {
        const FormatDesc d{"flac", {".flac"}, false, 8, 0, false, "1-8 channels; no height layouts; fixed 24-bit."};
        json j = base_format_json(d, true, {});
        j["bit_depths"] = json::array({"i24"});
        formats.push_back(std::move(j));
    }
    // opus_mka — always available; 48 kHz input, VBR per channel.
    {
        const FormatDesc d{"opus_mka", {".mka"}, true, 255, 48000, true, "48 kHz input required."};
        json j = base_format_json(d, true, {});
        j["bitrate_kbps_per_ch"] = bitrate_range(6, 320);
        formats.push_back(std::move(j));
    }
    // apac — macOS only; total target bitrate.
    {
        const FormatDesc d{"apac",
                           {".m4a", ".mp4"},
                           true,
                           24,
                           0,
                           true,
                           "macOS only; spatial/HOA default bitrate scales from 7.1.4 = 2048 kbps."};
        json j = base_format_json(d, apac, "macOS only (AudioToolbox)");
        j["bitrate_kbps_total"] = bitrate_range(64, 12000);
        formats.push_back(std::move(j));
    }
    // iamf — requires MR_ADM_ENABLE_IAMF; raw OBU stream.
    {
        const FormatDesc d{
            "iamf", {".iamf"}, true, 12, 48000, true, "Raw OBU stream (Opus); layouts up to 7.1.4 (9.1.6 disabled)."};
        formats.push_back(base_format_json(d, iamf, "requires MR_ADM_ENABLE_IAMF=ON and the AOM iamf-tools bridge"));
    }
    // iamf_mp4 — requires IAMF build plus an MP4 packager in PATH.
    {
        const FormatDesc d{
            "iamf_mp4", {".mp4"}, true, 12, 48000, true, "ISOBMFF-packaged IAMF; select via --iamf-container mp4."};
        const std::string_view reason =
            !iamf ? std::string_view{"requires MR_ADM_ENABLE_IAMF=ON and the AOM iamf-tools bridge"}
                  : std::string_view{"requires mp4box (GPAC) or ffmpeg in PATH"};
        formats.push_back(base_format_json(d, iamf_mp4, reason));
    }

    root["formats"] = std::move(formats);
    return root.dump(2);
}

} // namespace mradm::engine
