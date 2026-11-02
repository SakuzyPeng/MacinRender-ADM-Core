// Private render-orchestration boundary (mradm-engine, PCM consistency 5.2): the Rust backends'
// capability records and the discovery JSON documents (5.2 A), the request resolution (5.2 B), and the
// backend preparation and offline render step (5.2 C1). Backend records are process-lifetime Rust data
// and are never freed; a document is owned by its MradmEngineText handle.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../adm_core/config_ffi.h"
#include "../adm_metadata/adm_ffi.h"
#include "../adm_metadata/scene_ffi.h"
#include "routing_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmEngineText MradmEngineText;

// flags: 1 3D, 2 supports spread, 4 binaural.
typedef struct MradmEngineLayout {
    MradmAdmText id;
    MradmAdmText display_name;
    uint16_t channel_count;
    uint16_t lfe_count;
    uint32_t flags;
} MradmEngineLayout;

typedef struct MradmEngineMode {
    MradmAdmText id;
    MradmAdmText display_name;
    const MradmAdmText* layouts;
    size_t layouts_len;
    const uint32_t* sample_rates;
    size_t sample_rates_len;
    uint32_t supports_realtime;
} MradmEngineMode;

// flags: 1 objects, 2 DirectSpeakers, 4 HOA, 8 channelLock, 16 divergence, 32 screenRef,
// 64 diffuse, 128 render window.
typedef struct MradmEngineBackend {
    MradmAdmText renderer;
    MradmAdmText name;
    MradmAdmText version;
    const MradmEngineLayout* layouts;
    size_t layouts_len;
    const MradmEngineMode* modes;
    size_t modes_len;
    const MradmEngineMode* qualities;
    size_t qualities_len;
    const MradmAdmText* hrtf_sources;
    size_t hrtf_sources_len;
    uint32_t flags;
} MradmEngineBackend;

typedef struct MradmEngineSpatialLayout {
    MradmAdmText id;
    MradmAdmText display_name;
    uint16_t channel_count;
    uint16_t reserved;
} MradmEngineSpatialLayout;

// Island facts: apple_lfe_routing_ok is -1 when absent; apple may be NULL.
typedef struct MradmEnginePlatform {
    uint32_t apac;
    uint32_t iamf;
    uint32_t iamf_mp4_packager;
    int32_t apple_lfe_routing_ok;
    const MradmEngineBackend* apple;
    const MradmEngineSpatialLayout* system_spatial;
    size_t system_spatial_len;
} MradmEnginePlatform;

// index: 0 ear, 1 saf, 2 triple-balance, 3 hoa, 4 saf-binaural, 5 room-binaural.
int32_t mradm_engine_backend(uint32_t index, const MradmEngineBackend** out);
// kind: 0 capabilities, 1 layouts, 2 output formats, 3 input layouts, 4 render support matrix.
int32_t mradm_engine_document(uint32_t kind,
                              const MradmEnginePlatform* platform,
                              MradmEngineText** out,
                              MradmAdmText* document);
void mradm_engine_text_destroy(MradmEngineText* text);

// ── Request resolution (5.2 B) ──────────────────────────────────────────────────────────────
typedef struct MradmEngineResult MradmEngineResult;

typedef struct MradmEngineLog {
    uint32_t level;
    MradmAdmText module;
    MradmAdmText message;
} MradmEngineLog;

// code 0 is success. Every view stays valid until mradm_engine_result_destroy; the caller emits the
// log lines first, in order, whether or not the call succeeded.
typedef struct MradmEngineStatus {
    int32_t code;
    MradmAdmText message;
    MradmAdmText context;
    const MradmEngineLog* logs;
    size_t logs_len;
} MradmEngineStatus;

typedef struct MradmEngineRenderer {
    uint32_t selected;
    uint32_t backend;
    MradmAdmText effective_layout;
} MradmEngineRenderer;

typedef struct MradmEngineMatrixRequest {
    uint32_t mode;
    uint32_t has_json;
    MradmAdmText json;
    uint32_t has_path;
    MradmAdmText path;
    MradmAdmText path_label;
} MradmEngineMatrixRequest;

typedef struct MradmEngineTrim {
    uint32_t has_trim;
    uint64_t start_frame;
    uint64_t frame_count;
} MradmEngineTrim;

// bit_depth: 0 f32, 1 i24, 2 i16; packager: 0 none, 1 mp4box, 2 ffmpeg; channels: -1 unknown.
typedef struct MradmEngineOutputRequest {
    MradmAdmText extension;
    MradmAdmText layout;
    uint32_t bit_depth;
    uint32_t apac_caf;
    uint32_t iamf_mp4;
    const MradmAdmText* iamf_layers;
    size_t iamf_layers_len;
    int32_t channels;
    uint32_t iamf_available;
    uint32_t packager;
    int32_t ffmpeg_major;
} MradmEngineOutputRequest;

// The canonical layout id (static text) or the input itself.
int32_t mradm_engine_normalize_layout(const uint8_t* layout, size_t len, MradmAdmText* out);
// Values follow RendererSelection; diagnostics are the status log lines (empty module).
int32_t mradm_engine_resolve_renderer(uint32_t selection,
                                      const uint8_t* layout,
                                      size_t layout_len,
                                      uint32_t allow_speaker_stereo,
                                      uint32_t apple,
                                      MradmEngineRenderer* renderer,
                                      MradmEngineResult** out,
                                      MradmEngineStatus* status);
int32_t mradm_engine_validate_routing(uint32_t backend,
                                      const uint8_t* layout,
                                      size_t layout_len,
                                      uint32_t mode,
                                      MradmEngineResult** out,
                                      MradmEngineStatus* status);
// has_matrix 0: no matrix was requested; the matrix view belongs to the result.
int32_t mradm_engine_resolve_matrix(const MradmSceneHandle* scene,
                                    const MradmEngineMatrixRequest* request,
                                    const uint8_t* layout,
                                    size_t layout_len,
                                    uint32_t* has_matrix,
                                    MradmDirectMatrixView* matrix,
                                    MradmEngineResult** out,
                                    MradmEngineStatus* status);
int32_t mradm_engine_trim_window(double start_sec,
                                 uint32_t has_end,
                                 double end_sec,
                                 uint32_t sample_rate,
                                 uint64_t frames,
                                 MradmEngineTrim* trim,
                                 MradmEngineResult** out,
                                 MradmEngineStatus* status);
// targets bits: 1 FLAC, 2 Opus MKA, 4 APAC MPEG-4, 8 APAC CAF, 16 IAMF, 32 IAMF MP4. The normalized
// IAMF layers follow the log lines as entries with module "iamf-layer".
int32_t mradm_engine_output_target(const MradmEngineOutputRequest* request,
                                   uint32_t* targets,
                                   MradmEngineResult** out,
                                   MradmEngineStatus* status);
void mradm_engine_result_destroy(MradmEngineResult* result);

// ── Backend preparation and the offline render step (5.2 C1) ──────────────────────────────────

// RenderPlan for a backend: kind is render_common::EngineBackend; routing, speaker_spread and
// binaural_spread the C++ enum values; reference holds the default geometry when absent; sofa_path is
// UTF-8; room_profile may be null.
typedef struct MradmEnginePrepareRequest {
    uint32_t kind;
    MradmAdmText layout_id;
    uint32_t apple_geometry;
    uint32_t routing;
    uint32_t has_matrix;
    const MradmRenderMatrixRoute* matrix;
    size_t matrix_len;
    uint32_t speaker_spread;
    uint32_t binaural_spread;
    uint32_t triple_balance_d;
    uint32_t split_power;
    uint32_t smoothing_frames;
    uint32_t default_interp_ms;
    MradmAdmText input_path;
    uint32_t has_sofa;
    MradmAdmText sofa_path;
    MradmAdmText sofa_file_name;
    uint32_t has_reference;
    MradmRoomScreenGeometry reference;
    uint32_t has_reproduction;
    MradmRoomScreenGeometry reproduction;
    const void* room_profile;
    MradmAdmText room_hrir;
    double room_metres;
    uint32_t room_quality;
} MradmEnginePrepareRequest;

// The status and log lines, the Triple Balance report (has_report, also of a rejection) and the
// plan's counts; valid while the handle lives.
typedef struct MradmEnginePreparedView {
    MradmEngineStatus status;
    uint32_t has_report;
    MradmAdmText report;
    size_t tracks;
    size_t inputs;
    size_t outputs;
    uint32_t sample_rate;
} MradmEnginePreparedView;

// Unless the arguments are invalid (1, *output null) *output and *view are set, also for a rejected
// preparation; destroy with mradm_render_prepared_destroy.
int32_t mradm_engine_prepare(const MradmSceneHandle* scene,
                             const MradmEnginePrepareRequest* request,
                             MradmRenderPrepared** output,
                             MradmEnginePreparedView* view);

typedef struct MradmEngineOffline MradmEngineOffline;

// input_path / output_path are the UTF-8 paths the files open with; the display texts appear in
// locators and logs.
typedef struct MradmEngineOfflineSettings {
    MradmAdmText input_path;
    MradmAdmText output_path;
    MradmAdmText input_display;
    MradmAdmText output_display;
    MradmAdmText output_layout;
    size_t inputs;
    uint64_t frames;
    uint32_t sample_rate;
    uint32_t default_interp_ms;
    uint32_t smoothing_frames;
    uint32_t has_window;
    uint64_t window_start;
    uint64_t window_count;
    uint32_t has_meter_window;
    uint64_t meter_start;
    uint64_t meter_count;
    float pose[3];
} MradmEngineOfflineSettings;

// kind 0 nothing, 1 progress (stage .. message), 2 log (level, module, message), 3 done (the metrics);
// the texts live until the next call.
typedef struct MradmEngineOfflineEvent {
    uint32_t kind;
    uint32_t stage;
    uint32_t operation;
    double fraction;
    double stage_fraction;
    uint64_t current;
    uint64_t total;
    uint32_t level;
    MradmAdmText module;
    MradmAdmText message;
    uint32_t has_lufs;
    uint32_t has_peak;
    double lufs;
    double peak_dbtp;
} MradmEngineOfflineEvent;

// The offline render of a successful preparation; nothing opens before the first next.
int32_t mradm_engine_offline_open(const MradmRenderPrepared* prepared,
                                  const MradmEngineOfflineSettings* settings,
                                  MradmEngineOffline** output);
// cancelled is the caller's stop token now; an error (the status code, *status valid until the next
// call) or the done event spends the run.
int32_t mradm_engine_offline_next(MradmEngineOffline* handle,
                                  uint32_t cancelled,
                                  MradmEngineOfflineEvent* event,
                                  MradmEngineStatus* status);
void mradm_engine_offline_destroy(MradmEngineOffline* handle);

#ifdef __cplusplus
}
#endif
