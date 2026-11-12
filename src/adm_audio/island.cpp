#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "adm/audio_io.h"

#include "island_driver.h"
#include "island_ffi.h"

namespace mradm::audio {
namespace {

using island::fail;

void done(MradmEngineIslandReply& reply, const Result<void>& result) {
    if (!result) {
        fail(reply, result.error());
    }
}

} // namespace
} // namespace mradm::audio

extern "C" int32_t mradm_audio_island(const MradmEngineRenderEvent* request,
                                      const MradmIslandContext* context,
                                      MradmEngineIslandReply* reply) {
    using namespace mradm;
    using namespace mradm::audio;
    using island::Cancel;
    using island::native;
    using island::Progress;
    using island::text;
    if (request == nullptr || context == nullptr || reply == nullptr) {
        return 1;
    }
    const auto& e = *request;
    if (e.island < 1U || e.island > 9U || e.island == 2U || e.island == 3U) {
        return 2; // Not an audio island (the Apple backend has its own).
    }
    try {
        *reply = {};
        reply->ffmpeg_major = -1;
        std::optional<Cancel> cancel; // The probe and the metadata take no stop token.
        if (e.island >= 4U && e.island <= 8U) {
            cancel.emplace(*context);
        }
        const auto token = cancel ? cancel->token() : std::stop_token{};
        Progress progress(*context);
        const auto src = native(e.src.utf8);
        const auto dst = native(e.dst.utf8);
        const auto layout = text(e.layout);
        switch (e.island) {
        case 1: {
            const auto packager = detect_iamf_mp4_packager();
            reply->packager = static_cast<uint32_t>(packager.kind);
            reply->ffmpeg_major = packager.ffmpeg_major;
            break;
        }
        case 4:
            done(*reply, convert_to_flac(src, dst, token, &progress));
            break;
        case 5:
            done(*reply, convert_to_opus_mka(src, dst, layout, e.bitrate, token));
            break;
        case 6:
            done(*reply,
                 convert_to_apac(src,
                                 dst,
                                 layout,
                                 e.bitrate,
                                 e.drc_music != 0U,
                                 e.caf != 0U ? ApacContainer::caf : ApacContainer::mpeg4,
                                 token,
                                 &progress));
            break;
        case 7: {
            std::vector<std::string> layers;
            for (const auto& layer : std::span{e.layers, e.layers_len}) {
                layers.push_back(text(layer));
            }
            done(*reply,
                 convert_to_iamf(src,
                                 dst,
                                 layout,
                                 e.bitrate,
                                 e.has_lufs != 0U ? std::optional<double>(e.lufs) : std::nullopt,
                                 e.has_peak != 0U ? std::optional<double>(e.peak_dbtp) : std::nullopt,
                                 token,
                                 layers));
            break;
        }
        case 8:
            done(*reply, package_iamf_to_mp4(src, dst, token));
            break;
        default: {
            MetadataFields fields;
            fields.encoder = text(e.encoder);
            fields.date_utc = text(e.date_utc);
            fields.renderer = text(e.renderer);
            fields.output_layout = layout;
            if (e.has_lufs != 0U) {
                fields.lufs = e.lufs;
            }
            if (e.has_peak != 0U) {
                fields.peak_dbtp = e.peak_dbtp;
            }
            done(*reply, write_file_metadata(dst, fields));
            break;
        }
        }
    } catch (...) {
        fail(*reply, {ErrorCode::internal_error, "render island failed", {}});
    }
    return 0;
}
