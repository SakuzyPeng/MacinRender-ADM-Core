// Rust mradm-render (PCM consistency 4.5): the DirectSpeakers, speaker-label and LFE routing
// rules, the offline driver stepped block by block (next, write, commit, finish), and the VBAP,
// HOA and EAR backends with their file streams. Inputs are borrowed for the call; views live until the
// handle's next call or destroy. Error buffers: capacity in *len, bytes written out,
// NUL-terminated. Status values follow adm::ErrorCode.
#pragma once
#include "../adm_metadata/scene_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRenderMatrix MradmRenderMatrix;
typedef struct MradmRenderOffline MradmRenderOffline;
typedef struct MradmRenderPrepared MradmRenderPrepared;
typedef struct MradmRenderStream MradmRenderStream;

typedef struct MradmRenderTarget {
    MradmAdmText label;
    uint32_t is_lfe;
} MradmRenderTarget;

typedef struct MradmRenderMatrixTarget {
    MradmAdmText label;
    float gain;
} MradmRenderMatrixTarget;

typedef struct MradmRenderMatrixRoute {
    MradmAdmText source_label;
    uint32_t mute;
    const MradmRenderMatrixTarget* targets;
    size_t targets_len;
} MradmRenderMatrixRoute;

typedef struct MradmRenderResolvedTarget {
    size_t output;
    float gain;
} MradmRenderResolvedTarget;

typedef struct MradmRenderResolvedRoute {
    MradmAdmText source_label;
    MradmAdmText source_key;
    uint32_t mute;
    const MradmRenderResolvedTarget* targets;
    size_t targets_len;
} MradmRenderResolvedRoute;

typedef struct MradmRenderMatrixView {
    const MradmRenderResolvedRoute* routes;
    size_t routes_len;
} MradmRenderMatrixView;

typedef struct MradmRenderOverride {
    MradmAdmText object_id;
    MradmAdmText speaker_label;
    uint32_t mute;
    float gain_db;
    uint32_t has_head_locked;
    uint32_t head_locked;
} MradmRenderOverride;

// level: 1 info, 2 warning (adm::LogLevel); message lives as long as its owner.
typedef struct MradmRenderLogLine {
    uint32_t level;
    MradmAdmText message;
} MradmRenderLogLine;

typedef struct MradmRenderLfeRouting {
    uint32_t applies_to_22_2;
    uint32_t split_power;
    uint32_t has_lfe1;
    uint32_t has_lfe2;
    MradmRenderLogLine logs[2];
    size_t logs_len;
} MradmRenderLfeRouting;

typedef struct MradmRenderOfflineSettings {
    size_t inputs;
    size_t outputs;
    uint64_t frames;
    uint32_t sample_rate;
    uint32_t default_interp_ms;
    uint64_t smoothing_frames;
    uint32_t has_window;
    uint32_t stateful;
    uint64_t window_start;
    uint64_t window_count;
    uint32_t has_meter_window;
    uint64_t meter_start;
    uint64_t meter_count;
    uint32_t kernel; // 0: plan is a PcmMixer plan prepared in C++; 1: a Rust preparation's plan
} MradmRenderOfflineSettings;

// frames is 0 after the last block.
typedef struct MradmRenderOfflineBlock {
    uint64_t start;
    size_t frames;
    uint32_t last;
    size_t emit_offset;
    size_t emit_frames;
    const float* input;
    size_t input_len;
    float* output;
    size_t output_len;
} MradmRenderOfflineBlock;

typedef struct MradmRenderOfflineProgress {
    double fraction;
    double stage_fraction;
    uint64_t done;
    uint64_t total;
} MradmRenderOfflineProgress;

typedef struct MradmRenderOfflineMetrics {
    uint32_t has_lufs;
    uint32_t has_peak;
    double lufs;
    double peak_dbtp;
} MradmRenderOfflineMetrics;

typedef struct MradmRenderSpeaker {
    MradmAdmText label;
    float azimuth;
    float elevation;
    uint32_t is_lfe;
} MradmRenderSpeaker;

// routing: 0 label, 1 position, 2 matrix; matrix is read when has_matrix != 0.
typedef struct MradmRenderVbapRequest {
    const MradmRenderSpeaker* speakers;
    size_t speakers_len;
    MradmAdmText layout_id;
    MradmAdmText input_path;
    uint32_t spread;
    uint32_t routing;
    uint32_t has_matrix;
    const MradmRenderMatrixRoute* matrix;
    size_t matrix_len;
    uint32_t split_power;
} MradmRenderVbapRequest;

typedef struct MradmRenderHoaRequest {
    MradmAdmText layout_id;
    uint32_t split_power;
} MradmRenderHoaRequest;

// geometry: 0 standard, 1 Apple; matrix_routing selects matrix DirectSpeakers routing (otherwise
// automatic), and matrix is read when has_matrix != 0.
typedef struct MradmRenderEarRequest {
    MradmAdmText layout_id;
    uint32_t geometry;
    MradmAdmText input_path;
    uint32_t matrix_routing;
    uint32_t has_matrix;
    const MradmRenderMatrixRoute* matrix;
    size_t matrix_len;
    uint32_t split_power;
} MradmRenderEarRequest;

// The error text of a failed preparation; plan (the offline driver's plan) is null unless prepared.
typedef struct MradmRenderPreparedView {
    const MradmRenderLogLine* logs;
    size_t logs_len;
    MradmAdmText message;
    MradmAdmText context;
    const void* plan;
    size_t tracks;
    size_t outputs;
} MradmRenderPreparedView;
// cppcheck-suppress-end unusedStructMember

// kind: 0 alphanumeric key, 1 canonical label, 2 canonical label with aliases resolved.
// *output_len is the capacity in (at least raw_len + 8) and the length out.
int32_t mradm_render_label(uint32_t kind, const uint8_t* raw, size_t raw_len, uint8_t* output, size_t* output_len);
uint32_t mradm_render_is_lfe(const uint8_t* raw, size_t raw_len);
// 0 none, 1 LFE1, 2 LFE2.
uint32_t mradm_render_lfe_target(const MradmAdmText* labels, size_t labels_len, uint32_t low_pass);
uint32_t mradm_render_target_index(const MradmRenderTarget* targets_ptr,
                                   size_t targets_len,
                                   const MradmAdmText* labels,
                                   size_t labels_len,
                                   size_t* index);
uint32_t mradm_render_label_position(const MradmAdmText* labels, size_t labels_len, float* azimuth, float* elevation);
int32_t mradm_render_matrix_resolve(const MradmRenderMatrixRoute* routes,
                                    size_t routes_len,
                                    const MradmRenderTarget* targets_ptr,
                                    size_t targets_len,
                                    MradmAdmText layout_id,
                                    MradmRenderMatrix** output,
                                    MradmRenderMatrixView* view,
                                    uint8_t* message,
                                    size_t* message_len,
                                    uint8_t* context,
                                    size_t* context_len);
void mradm_render_matrix_destroy(MradmRenderMatrix* handle);
int32_t mradm_render_matrix_route(const MradmRenderMatrix* matrix,
                                  const MradmAdmText* labels,
                                  size_t labels_len,
                                  size_t* index,
                                  uint8_t* message,
                                  size_t* message_len,
                                  uint8_t* context,
                                  size_t* context_len);
// Bit 1: *gain written; bit 2: *head_locked written; otherwise the channel inherits.
uint32_t mradm_render_live_override(const MradmRenderOverride* values,
                                    size_t values_len,
                                    MradmAdmText object_id,
                                    MradmAdmText key,
                                    float* gain,
                                    uint32_t* head_locked);
// output is filled even on failure: its log lines precede the error.
int32_t mradm_render_lfe_routing(uint32_t split_power,
                                 MradmAdmText output_layout,
                                 const uint32_t* targets_ptr,
                                 size_t targets_len,
                                 MradmRenderLfeRouting* output,
                                 uint8_t* message,
                                 size_t* message_len);
float mradm_render_lfe_gain(const MradmRenderLfeRouting* routing, uint32_t input_target, uint32_t output_target);

// Driver errors set *location: 0 none, 1 path=<input>, 2 input=<input>, 3 meter, 4 HOA DSP.
int32_t mradm_render_offline_open(const uint8_t* path,
                                  size_t path_len,
                                  const void* plan,
                                  const MradmRenderOfflineSettings* settings,
                                  MradmAdmText backend,
                                  MradmRenderOffline** output,
                                  uint32_t* location,
                                  uint8_t* message,
                                  size_t* message_len);
int32_t mradm_render_offline_next(MradmRenderOffline* handle,
                                  MradmRenderOfflineBlock* block,
                                  uint32_t* location,
                                  uint8_t* message,
                                  size_t* message_len);
int32_t mradm_render_offline_commit(MradmRenderOffline* handle,
                                    MradmRenderOfflineProgress* progress,
                                    uint32_t* location,
                                    uint8_t* message,
                                    size_t* message_len);
int32_t mradm_render_offline_finish(MradmRenderOffline* handle,
                                    MradmRenderOfflineMetrics* metrics,
                                    uint32_t* location,
                                    uint8_t* message,
                                    size_t* message_len);
void mradm_render_offline_destroy(MradmRenderOffline* handle);

// *output and *view are set unless the arguments are invalid: the log lines precede the error.
int32_t mradm_render_vbap_prepare(const MradmSceneHandle* scene,
                                  const MradmRenderVbapRequest* request,
                                  MradmRenderPrepared** output,
                                  MradmRenderPreparedView* view);
int32_t mradm_render_hoa_prepare(const MradmSceneHandle* scene,
                                 const MradmRenderHoaRequest* request,
                                 MradmRenderPrepared** output,
                                 MradmRenderPreparedView* view);
int32_t mradm_render_ear_prepare(const MradmSceneHandle* scene,
                                 const MradmRenderEarRequest* request,
                                 MradmRenderPrepared** output,
                                 MradmRenderPreparedView* view);
void mradm_render_prepared_destroy(MradmRenderPrepared* handle);
// Stream errors set *location like the offline driver's.
int32_t mradm_render_stream_open(const MradmRenderPrepared* prepared,
                                 const uint8_t* path,
                                 size_t path_len,
                                 uint32_t default_interp_ms,
                                 uint64_t smoothing_frames,
                                 MradmRenderStream** output,
                                 uint32_t* location,
                                 uint8_t* message,
                                 size_t* message_len);
int32_t mradm_render_stream_process(MradmRenderStream* handle,
                                    float* output,
                                    size_t output_len,
                                    size_t frames,
                                    size_t* produced,
                                    uint32_t* location,
                                    uint8_t* message,
                                    size_t* message_len);
int32_t mradm_render_stream_seek(
    MradmRenderStream* handle, uint64_t frame, uint32_t* location, uint8_t* message, size_t* message_len);
int32_t mradm_render_stream_overrides(MradmRenderStream* handle, const MradmRenderOverride* values, size_t values_len);
void mradm_render_stream_destroy(MradmRenderStream* handle);

#ifdef __cplusplus
}
#endif
