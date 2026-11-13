#include "apac_worker_island.h"

#include <cstdio>

#include "audio_io_internal.h"

extern "C" int32_t mradm_audio_apac_child(
    const char* src, const char* dst, const char* layout, uint32_t bitrate, uint32_t drc_music, uint32_t caf) {
    try {
        auto result = mradm::audio::run_apac_encode_child(src, dst, layout, bitrate, drc_music != 0U, caf != 0U);
        if (result.has_value()) {
            return 0;
        }
        // The parent's watchdog rebuilds the error from this line; the P / F heartbeat lines were
        // already streamed by the encoder.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::printf("E %d %s\n", static_cast<int>(result.error().code), result.error().message.c_str());
        std::fflush(stdout);
        return 1;
    } catch (...) {
        return 1;
    }
}
