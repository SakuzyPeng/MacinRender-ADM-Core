#include <stop_token>

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
    if (e.island != 6U) {
        return 2; // Only APAC is a C++ audio island; the others are in Rust or the Apple island.
    }
    try {
        *reply = {};
        reply->ffmpeg_major = -1;
        const Cancel cancel(*context);
        const auto token = cancel.token();
        Progress progress(*context);
        const auto src = native(e.src.utf8);
        const auto dst = native(e.dst.utf8);
        done(*reply,
             convert_to_apac(src,
                             dst,
                             text(e.layout),
                             e.bitrate,
                             e.drc_music != 0U,
                             e.caf != 0U ? ApacContainer::caf : ApacContainer::mpeg4,
                             token,
                             &progress));
    } catch (...) {
        fail(*reply, {ErrorCode::internal_error, "render island failed", {}});
    }
    return 0;
}
