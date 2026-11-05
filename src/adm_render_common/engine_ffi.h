// Private render-orchestration boundary (mradm-engine, PCM consistency 5.2): the Rust backends'
// capability records and the discovery JSON documents (5.2 A), the request resolution (5.2 B), the
// backend preparation and offline render step (5.2 C1), and the offline render service (5.2 C2).
// Backend records are process-lifetime Rust data and are never freed; a document is owned by its
// MradmEngineText handle.
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

// The canonical layout id (static text) or the input itself.
int32_t mradm_engine_normalize_layout(const uint8_t* layout, size_t len, MradmAdmText* out);
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

// ── File streaming (5.3 A) ─────────────────────────────────────────────────────────────────────
// One stream for every Rust backend. validate may overlap the worker; serialize the other calls
// and destroy after all return. Errors set *location: 0 none, 1 path=<input>, 2 input=<input>,
// 3 meter, 4 HOA DSP, 5 Triple Balance DSP, 6 room-binaural.
typedef struct MradmEngineStream MradmEngineStream;

// RenderPlan's input format and controls; spread 0 cloud, 1 none, 2 SAF spreader.
typedef struct MradmEngineStreamSettings {
    size_t channels;
    uint64_t frames;
    uint32_t sample_rate;
    uint32_t default_interp_ms;
    uint32_t smoothing_frames;
    uint32_t spread;
    float pose[3];
} MradmEngineStreamSettings;

// binaural: the output layout is "binaural" rather than the plan's.
typedef struct MradmEngineStreamInfo {
    size_t outputs;
    uint32_t sample_rate;
    uint32_t binaural;
    uint32_t requires_preroll;
} MradmEngineStreamInfo;

int32_t mradm_engine_stream_open(const MradmRenderPrepared* prepared,
                                 const uint8_t* path,
                                 size_t path_len,
                                 const MradmEngineStreamSettings* settings,
                                 MradmEngineStream** output,
                                 MradmEngineStreamInfo* info,
                                 uint32_t* location,
                                 uint8_t* message,
                                 size_t* message_len);
int32_t mradm_engine_stream_process(MradmEngineStream* handle,
                                    float* output,
                                    size_t output_len,
                                    size_t frames,
                                    size_t* produced,
                                    uint32_t* location,
                                    uint8_t* message,
                                    size_t* message_len);
// cancelled is the stop token sampled before the call; *pending asks for another seek_step.
int32_t mradm_engine_stream_seek_begin(MradmEngineStream* handle,
                                       uint64_t frame,
                                       uint32_t cancelled,
                                       uint32_t* pending,
                                       uint32_t* location,
                                       uint8_t* message,
                                       size_t* message_len);
int32_t mradm_engine_stream_seek_step(MradmEngineStream* handle,
                                      uint32_t cancelled,
                                      uint32_t* pending,
                                      uint32_t* location,
                                      uint8_t* message,
                                      size_t* message_len);
int32_t mradm_engine_stream_validate(const MradmEngineStream* handle,
                                     const MradmRenderLiveOverride* values,
                                     size_t values_len,
                                     uint8_t* message,
                                     size_t* message_len,
                                     uint8_t* context,
                                     size_t* context_len);
int32_t
mradm_engine_stream_overrides(MradmEngineStream* handle, const MradmRenderLiveOverride* values, size_t values_len);
void mradm_engine_stream_pose(MradmEngineStream* handle, float yaw, float pitch, float roll);
uint64_t mradm_engine_stream_tail(const MradmEngineStream* handle);
// A new room control handle; null for another backend.
MradmRenderRoomControl* mradm_engine_stream_control(const MradmEngineStream* handle);
void mradm_engine_stream_destroy(MradmEngineStream* handle);

// ── The offline render service (5.2 C2) ───────────────────────────────────────────────────────

// A path's UTF-8 text and its spelling in messages.
typedef struct MradmEnginePath {
    MradmAdmText utf8;
    MradmAdmText display;
} MradmEnginePath;

// RenderRequest with its RenderOptions; the enum fields are the C++ values; room_profile may be null.
typedef struct MradmEngineRenderRequest {
    MradmEnginePath input;
    uint32_t has_output;
    MradmEnginePath output;
    uint32_t renderer;
    MradmAdmText output_layout;
    uint32_t has_input_layout;
    MradmAdmText input_layout;
    const MradmAdmText* input_channel_labels;
    size_t input_channel_labels_len;
    uint32_t measure_loudness;
    float loudness_target_lufs;
    uint32_t peak_limit;
    float peak_limit_dbtp;
    uint32_t peak_normalize_to_limit;
    double final_gain_db;
    uint32_t output_bit_depth;
    uint32_t opus_bitrate_per_ch_kbps;
    uint32_t apac_bitrate_kbps;
    uint32_t apac_drc_music;
    uint32_t apac_caf;
    uint32_t has_sofa;
    MradmEnginePath sofa;
    MradmAdmText sofa_file_name;
    uint32_t has_policy_path;
    MradmEnginePath policy_path;
    uint32_t has_policy_json;
    MradmAdmText policy_json;
    uint32_t has_matrix_path;
    MradmEnginePath matrix_path;
    uint32_t has_matrix_json;
    MradmAdmText matrix_json;
    uint32_t has_report_path;
    MradmEnginePath report_path;
    uint32_t capture_report;
    uint32_t default_interp_ms;
    uint32_t object_smoothing_frames;
    uint32_t apple_geometry;
    uint32_t routing;
    uint32_t speaker_spread;
    uint32_t triple_balance_mode;
    uint32_t binaural_spread;
    uint32_t split_power;
    uint32_t apple_spatial_preset;
    uint32_t apple_speaker_rendering_flags;
    float listener_orientation[3];
    double render_start_sec;
    uint32_t has_end;
    double render_end_sec;
    const void* room_profile;
    MradmAdmText room_hrir;
    double room_metres;
    uint32_t room_quality;
    uint32_t has_reference;
    MradmRoomScreenGeometry reference;
    uint32_t has_reproduction;
    MradmRoomScreenGeometry reproduction;
    uint32_t internal_allow_speaker_stereo;
    uint32_t iamf_mp4;
    const MradmAdmText* iamf_layers;
    size_t iamf_layers_len;
} MradmEngineRenderRequest;

typedef struct MradmEngineProgress {
    uint32_t stage;
    uint32_t operation;
    double fraction;
    double stage_fraction;
    uint64_t current;
    uint64_t total;
    MradmAdmText message;
} MradmEngineProgress;

// kind 0 nothing, 1 progress, 2 log (level, module, message), 3 island, 4 done. Islands: 1 MP4
// packager probe, 2 Apple prepare, 3 Apple render, 4 FLAC, 5 Opus MKA, 6 APAC, 7 IAMF, 8 MP4
// packaging, 9 metadata; src / dst the files (the Apple plan's output in dst).
typedef struct MradmEngineRenderEvent {
    uint32_t kind;
    MradmEngineProgress progress;
    uint32_t level;
    MradmAdmText module;
    MradmAdmText message;
    uint32_t island;
    MradmEnginePath src;
    MradmEnginePath dst;
    MradmAdmText layout;
    uint32_t bitrate;
    uint32_t drc_music;
    uint32_t caf;
    uint32_t has_lufs;
    uint32_t has_peak;
    double lufs;
    double peak_dbtp;
    const MradmAdmText* layers;
    size_t layers_len;
    uint32_t has_window;
    uint64_t window_start;
    uint64_t window_count;
    uint32_t has_meter_window;
    uint64_t meter_start;
    uint64_t meter_count;
    uint32_t has_matrix;
    MradmDirectMatrixView matrix;
    MradmAdmText encoder;
    MradmAdmText date_utc;
    MradmAdmText renderer;
} MradmEngineRenderEvent;

// code 0 for success; the Apple render's metrics; packager 0 none, 1 mp4box, 2 ffmpeg.
typedef struct MradmEngineIslandReply {
    int32_t code;
    MradmAdmText message;
    MradmAdmText context;
    uint32_t has_lufs;
    uint32_t has_peak;
    double lufs;
    double peak_dbtp;
    uint32_t packager;
    int32_t ffmpeg_major;
} MradmEngineIslandReply;

typedef struct MradmEngineRenderResult {
    int32_t code;
    MradmAdmText message;
    MradmAdmText context;
    uint32_t has_output;
    MradmAdmText output_path;
    uint32_t has_metrics;
    uint32_t has_lufs;
    uint32_t has_peak;
    double lufs;
    double peak_dbtp;
    const MradmEngineLog* logs;
    size_t logs_len;
    uint32_t has_report;
    MradmAdmText report;
} MradmEngineRenderResult;

typedef struct MradmEngineRender MradmEngineRender;

// Optional preimported scene and independent cached Rust/Apple preparation. Invalid arguments
// (including non-UTF-8 or NUL-containing paths) return 1; panics return 6.
int32_t mradm_engine_render_begin(const MradmEngineRenderRequest* request,
                                  const MradmEnginePlatform* platform,
                                  const MradmSceneHandle* preimported,
                                  const MradmRenderPrepared* cached,
                                  uint32_t apple_cached,
                                  MradmEngineRender** output);
// cancelled is the caller's stop token now; texts live until the next call on the handle.
int32_t mradm_engine_render_next(MradmEngineRender* handle, uint32_t cancelled, MradmEngineRenderEvent* event);
int32_t mradm_engine_render_resume(MradmEngineRender* handle, const MradmEngineIslandReply* reply);
// An island's progress event mapped into the render's range.
int32_t mradm_engine_render_map_progress(const MradmEngineRender* handle,
                                         const MradmEngineProgress* event,
                                         MradmEngineProgress* output);
int32_t mradm_engine_render_result(MradmEngineRender* handle, MradmEngineRenderResult* output);
// The preparation the render made, or null; destroy with mradm_render_prepared_destroy.
MradmRenderPrepared* mradm_engine_render_take_prepared(MradmEngineRender* handle);
// A copy of the render's scene (the Apple island); destroy with mradm_scene_destroy.
MradmSceneHandle* mradm_engine_render_scene(const MradmEngineRender* handle);
void mradm_engine_render_destroy(MradmEngineRender* handle);
// Policy for preview/export: returns 0 with result/status also on policy errors; nonzero on boundary failures.
int32_t mradm_engine_apply_policy(MradmSceneHandle* scene,
                                  uint32_t has_json,
                                  MradmAdmText json,
                                  uint32_t has_path,
                                  MradmEnginePath path,
                                  MradmAdmText input_display,
                                  MradmEngineResult** result,
                                  MradmEngineStatus* status);

// ── The monitor's stream opening (5.3 B) ───────────────────────────────────────────────────────
typedef struct MradmEngineMonitor MradmEngineMonitor;

// kind 0 a rejection (status), 1 a stream (info and the plan's layout), 2 the Apple backend over
// layout and the matrix; the status logs come first either way. Texts live until destroy.
typedef struct MradmEngineMonitorView {
    MradmEngineStatus status;
    uint32_t kind;
    MradmEngineStreamInfo info;
    MradmAdmText layout;
    uint32_t has_matrix;
    MradmDirectMatrixView matrix;
} MradmEngineMonitorView;

// scene is the session's policy-applied scene (may be null). Returns 1 for invalid arguments
// (including paths not encoded as UTF-8 or containing NUL), 6 for a panic, otherwise 0.
int32_t mradm_engine_monitor_open(const MradmEngineRenderRequest* request,
                                  const MradmEnginePlatform* platform,
                                  const MradmSceneHandle* scene,
                                  uint32_t system_spatial,
                                  MradmEngineMonitor** output,
                                  MradmEngineMonitorView* view);
// The opened stream (kind 1, once), or null; destroy with mradm_engine_stream_destroy.
MradmEngineStream* mradm_engine_monitor_take_stream(MradmEngineMonitor* handle);
void mradm_engine_monitor_destroy(MradmEngineMonitor* handle);

#ifdef __cplusplus
}
#endif
