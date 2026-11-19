// The render-scene records of the Rust services (rust_services_ffi.h, mradm-scene): the scene view the Apple
// island reads. A view is borrowed until the next call that changes the scene or scene_destroy.
#pragma once
#include "adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmSceneImportInfo {
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t frames;
    uint32_t source_kind; // 0 = ADM, 1 = channel bed
    MradmAdmText input_layout;
    const MradmAdmText* input_channel_labels;
    size_t input_channel_labels_len;
} MradmSceneImportInfo;

typedef struct MradmSceneHandle MradmSceneHandle;

#ifdef __cplusplus
}
#endif
