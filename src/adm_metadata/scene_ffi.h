// Private render-scene import boundary (PCM consistency 4.3); see mradm-scene. Scene views are
// borrowed until mradm_scene_destroy; preset and catalog tables point at process-lifetime Rust data.
// Status values follow adm::ErrorCode. `context` receives the error locator the caller attaches:
// 0 none, 1 "input=<path>", 2 "path=<path>".
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

typedef struct MradmInputChannelRecord {
    MradmAdmText token;
    MradmAdmText speaker_label;
    float azimuth;
    float elevation;
    uint32_t is_lfe;
} MradmInputChannelRecord;

typedef struct MradmInputLayoutRecord {
    MradmAdmText id;
    MradmAdmText internal_id;
    MradmAdmText display_name;
    uint32_t wave_channel_mask;
    const MradmInputChannelRecord* channels;
    size_t channels_len;
    const MradmInputChannelRecord* wave_mask_channels;
    size_t wave_mask_channels_len;
} MradmInputLayoutRecord;

typedef struct MradmSceneHandle MradmSceneHandle;

// routed: 0 = ADM BW64 only; 1 = ADM or channel bed by the options. path is UTF-8.
int32_t mradm_scene_import(const uint8_t* path,
                           size_t path_len,
                           uint32_t routed,
                           uint32_t has_layout,
                           MradmAdmText layout,
                           const MradmAdmText* input_labels,
                           size_t input_labels_len,
                           MradmSceneHandle** output,
                           uint32_t* context,
                           uint8_t* message,
                           size_t capacity);
int32_t mradm_scene_view(
    const MradmSceneHandle* handle, MradmSceneImportInfo* info, MradmAdmView* view, uint8_t* message, size_t capacity);
void mradm_scene_destroy(MradmSceneHandle* handle);
// Writes render-scene/offline.<stage>.i32 of this scene (diagnostic builds only; otherwise a no-op).
int32_t mradm_scene_checkpoint(const MradmSceneHandle* handle, MradmAdmText stage, uint8_t* message, size_t capacity);

int32_t mradm_scene_input_layouts(const MradmInputLayoutRecord** entries,
                                  size_t* entries_len,
                                  uint8_t* message,
                                  size_t capacity);
int32_t mradm_scene_channel_catalog(const MradmInputChannelRecord** entries,
                                    size_t* entries_len,
                                    uint8_t* message,
                                    size_t capacity);
// indices must hold input_labels_len entries; each receives a catalog index.
int32_t mradm_scene_resolve_labels(const MradmAdmText* input_labels,
                                   size_t input_labels_len,
                                   uint32_t* indices,
                                   size_t indices_len,
                                   uint8_t* message,
                                   size_t capacity);

#ifdef __cplusplus
}
#endif
