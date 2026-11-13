// The APAC encode worker of the audio library, run by the `mradm __apac-encode` command.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Encodes src (UTF-8) to dst with layout id, writing the heartbeat protocol lines ("P", "F", and
// "E <code> <message>" on failure) to stdout; returns the exit code (0 or 1).
int32_t mradm_audio_apac_child(
    const char* src, const char* dst, const char* layout, uint32_t bitrate, uint32_t drc_music, uint32_t caf);

#ifdef __cplusplus
}
#endif
