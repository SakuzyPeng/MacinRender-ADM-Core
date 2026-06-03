#include "format_table.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/render_binaural.h"

namespace mradm::engine {

namespace {

using nlohmann::json;

// Per-channel Opus and total APAC bitrate ranges.
OutputFormatInfo with_bitrate(OutputFormatInfo info, bool per_channel, uint32_t min_kbps, uint32_t max_kbps) {
    info.has_bitrate = true;
    info.bitrate_per_channel = per_channel;
    info.bitrate_min_kbps = min_kbps;
    info.bitrate_max_kbps = max_kbps;
    return info;
}

} // namespace

OutputFormats build_output_formats() {
    const bool apac = audio::apac_encoding_available();
    const bool iamf = audio::iamf_encoding_available();
    const bool iamf_mp4 = iamf && audio::iamf_mp4_packager_available();

    OutputFormats out;
    out.features = {.apac = apac, .iamf = iamf, .iamf_mp4_packager = iamf_mp4, .sofa = binaural_sofa_supported()};

    // wav — always available; bit depth selectable.
    out.formats.push_back({.format = "wav",
                           .extensions = {".wav"},
                           .available = true,
                           .lossy = false,
                           .max_channels = 0,
                           .fixed_sample_rate = 0,
                           .supports_height = true,
                           .bit_depths = {"f32", "i24", "i16"},
                           .note = "Uncompressed PCM; not a preferred delivery format."});
    // caf — always available; float32, CoreAudio channel layouts.
    out.formats.push_back({.format = "caf",
                           .extensions = {".caf"},
                           .available = true,
                           .lossy = false,
                           .max_channels = 0,
                           .fixed_sample_rate = 0,
                           .supports_height = true,
                           .bit_depths = {"f32"},
                           .note = "Float32 CoreAudio container; richer metadata on macOS."});
    // flac — always available; 1-8 channels, no height, fixed 24-bit.
    out.formats.push_back({.format = "flac",
                           .extensions = {".flac"},
                           .available = true,
                           .lossy = false,
                           .max_channels = 8,
                           .fixed_sample_rate = 0,
                           .supports_height = false,
                           .bit_depths = {"i24"},
                           .note = "1-8 channels; no height layouts; fixed 24-bit."});
    // opus_mka — always available; 48 kHz input, VBR per channel.
    out.formats.push_back(with_bitrate({.format = "opus_mka",
                                        .extensions = {".mka"},
                                        .available = true,
                                        .lossy = true,
                                        .max_channels = 255,
                                        .fixed_sample_rate = 48000,
                                        .supports_height = true,
                                        .note = "48 kHz input required."},
                                       /*per_channel=*/true,
                                       6,
                                       320));
    // apac — macOS only; total target bitrate.
    out.formats.push_back(
        with_bitrate({.format = "apac",
                      .extensions = {".m4a", ".mp4", ".caf"},
                      .available = apac,
                      .available_reason = apac ? std::string{} : std::string{"macOS only (AudioToolbox)"},
                      .lossy = true,
                      .max_channels = 24,
                      .fixed_sample_rate = 48000,
                      .supports_height = true,
                      .note = "macOS only; .caf requires --apac-container caf; spatial/HOA default bitrate scales "
                              "from 7.1.4 = 2048 kbps."},
                     /*per_channel=*/false,
                     64,
                     12000));
    // iamf — requires MR_ADM_ENABLE_IAMF; raw OBU stream.
    out.formats.push_back({.format = "iamf",
                           .extensions = {".iamf"},
                           .available = iamf,
                           .available_reason = iamf ? std::string{}
                                                    : std::string{"requires MR_ADM_ENABLE_IAMF=ON and the AOM "
                                                                  "iamf-tools bridge"},
                           .lossy = true,
                           .max_channels = 12,
                           .fixed_sample_rate = 48000,
                           .supports_height = true,
                           .note = "Raw OBU stream (Opus); layouts up to 7.1.4 (9.1.6 disabled)."});
    // iamf_mp4 — requires IAMF build plus an MP4 packager in PATH.
    std::string iamf_mp4_reason;
    if (!iamf_mp4) {
        iamf_mp4_reason = !iamf ? std::string{"requires MR_ADM_ENABLE_IAMF=ON and the AOM iamf-tools bridge"}
                                : std::string{"requires mp4box (GPAC) or ffmpeg in PATH"};
    }
    out.formats.push_back({.format = "iamf_mp4",
                           .extensions = {".mp4"},
                           .available = iamf_mp4,
                           .available_reason = std::move(iamf_mp4_reason),
                           .lossy = true,
                           .max_channels = 12,
                           .fixed_sample_rate = 48000,
                           .supports_height = true,
                           .note = "ISOBMFF-packaged IAMF; select via --iamf-container mp4."});
    return out;
}

std::string output_formats_to_json() {
    const OutputFormats data = build_output_formats();

    json root = json::object();
    root["schema"] = "mradm.output-formats";
    root["schema_version"] = 1;

    json features = json::object();
    features["apac"] = data.features.apac;
    features["iamf"] = data.features.iamf;
    features["iamf_mp4_packager"] = data.features.iamf_mp4_packager;
    features["sofa"] = data.features.sofa;
    root["features"] = std::move(features);

    json formats = json::array();
    for (const auto& f : data.formats) {
        json j = json::object();
        j["format"] = f.format;
        j["extensions"] = f.extensions;
        j["available"] = f.available;
        if (!f.available && !f.available_reason.empty()) {
            j["available_reason"] = f.available_reason;
        }
        j["lossy"] = f.lossy;
        j["max_channels"] = f.max_channels;
        j["fixed_sample_rate"] = f.fixed_sample_rate;
        j["supports_height"] = f.supports_height;
        if (!f.note.empty()) {
            j["note"] = f.note;
        }
        if (!f.bit_depths.empty()) {
            j["bit_depths"] = f.bit_depths;
        }
        if (f.has_bitrate) {
            json range = json::object();
            range["min"] = f.bitrate_min_kbps;
            range["max"] = f.bitrate_max_kbps;
            range["auto"] = 0;
            j[f.bitrate_per_channel ? "bitrate_kbps_per_ch" : "bitrate_kbps_total"] = std::move(range);
        }
        formats.push_back(std::move(j));
    }
    root["formats"] = std::move(formats);
    return root.dump(2);
}

} // namespace mradm::engine
