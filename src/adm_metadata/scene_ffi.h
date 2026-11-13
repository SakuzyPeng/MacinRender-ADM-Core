// Private render-scene boundary (mradm-scene): the scene view the Apple island reads. A view is
// borrowed until the next call that changes the scene or mradm_scene_destroy. Status values follow
// adm::ErrorCode. The import, policy, report and export entries only tests call are in
// tests/support/ffi/scene_test_ffi.h.
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
int32_t mradm_scene_view(
    const MradmSceneHandle* handle, MradmSceneImportInfo* info, MradmAdmView* view, uint8_t* message, size_t capacity);
void mradm_scene_destroy(MradmSceneHandle* handle);

#ifdef __cplusplus
}
#endif
