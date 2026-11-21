// AudioToolbox calls made inside the APAC worker. A successful open returns an ExtAudioFile
// owned by the caller until finish. On failure it writes a NUL-terminated diagnostic.
#pragma once
#include "../adm_metadata/adm_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmApacEncoder {
    void* (*open)(MradmAdmText path,
                  uint32_t layout_tag,
                  uint32_t bitrate_kbps,
                  uint32_t drc_music,
                  uint32_t caf,
                  char* message,
                  size_t capacity);
    int32_t (*write)(void* file, float* interleaved, uint32_t channels, uint32_t frames);
    int32_t (*finish)(void* file);
} MradmApacEncoder;

const MradmApacEncoder* mradm_apple_apac_encoder(void);

#ifdef __cplusplus
}
#endif
