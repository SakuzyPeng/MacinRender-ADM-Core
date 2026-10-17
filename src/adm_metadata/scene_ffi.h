// Private render-scene boundary (PCM consistency 4.3, the semantic policy since 4.4); see mradm-scene.
// Scene views are borrowed until the next call that changes the scene (apply_policy, report,
// template) or mradm_scene_destroy; preset and catalog tables point at process-lifetime Rust data.
// Status values follow adm::ErrorCode. `context` receives the error locator the caller attaches:
// 0 none, 1 "input=<path>", 2 "path=<path>", 3 the caller's source label.
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

// user_mute: 0 unset, 1 false, 2 true.
typedef struct MradmSceneObjectState {
    float user_gain;
    uint32_t user_mute;
    uint32_t mute;
} MradmSceneObjectState;

typedef struct MradmSceneDirectState {
    float user_gain;
    uint32_t user_mute;
    uint32_t position_override;
    uint32_t has_position;
} MradmSceneDirectState;

// Aligned with the view's objects and direct_blocks.
typedef struct MradmSceneOverlay {
    const MradmSceneObjectState* objects;
    size_t objects_len;
    const MradmSceneDirectState* direct_blocks;
    size_t direct_blocks_len;
} MradmSceneOverlay;

typedef struct MradmSceneReportOptions {
    MradmAdmText renderer;
    MradmAdmText policy_path;
    uint32_t supports_channel_lock;
    uint32_t supports_object_divergence;
    uint32_t supports_diffuse;
    const MradmAdmText* hrtf_sources;
    size_t hrtf_sources_len;
    MradmAdmText renderer_effective;
} MradmSceneReportOptions;

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
int32_t
mradm_scene_overlay(const MradmSceneHandle* handle, MradmSceneOverlay* overlay, uint8_t* message, size_t capacity);
// Parses mradm.semantic-policy.v1 JSON and applies it; policy errors set context 3.
int32_t mradm_scene_apply_policy(MradmSceneHandle* handle,
                                 const uint8_t* json,
                                 size_t json_len,
                                 uint32_t* context,
                                 uint8_t* message,
                                 size_t capacity);
int32_t mradm_scene_policy_warnings(const MradmSceneHandle* handle,
                                    const MradmAdmText** warnings,
                                    size_t* warnings_len,
                                    uint8_t* message,
                                    size_t capacity);
// The semantic report and the policy template: nlohmann dump(2) bytes without a trailing newline,
// held by the handle until the next report or template call.
int32_t mradm_scene_report(MradmSceneHandle* handle,
                           const MradmSceneReportOptions* options,
                           MradmAdmText* output,
                           uint8_t* message,
                           size_t capacity);
int32_t mradm_scene_template(MradmSceneHandle* handle, MradmAdmText* output, uint8_t* message, size_t capacity);

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
