// The part of the private boundary in src/adm_render_common/engine_ffi.h that production C/C++
// does not use: tests and consistency tools call these mradm-ffi entries (some are also reached
// from Rust inside the workspace). Production keeps only what it calls.
//
// Private render-orchestration boundary (mradm-engine): the Rust backends'
// capability records and the discovery JSON documents, the request resolution, the backend
// preparation and offline render step, and the offline render service.
// Backend records are process-lifetime Rust data and are never freed; a document is owned by its
// MradmEngineText handle.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_render_common/engine_ffi.h"
#include "adm_test_ffi.h"
#include "config_test_ffi.h"
#include "dsp_test_ffi.h"
#include "routing_test_ffi.h"
#include "scene_test_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmEngineText MradmEngineText;
// A backend preparation (mradm_engine_prepare).
typedef struct MradmRenderPrepared MradmRenderPrepared;
void mradm_render_prepared_destroy(MradmRenderPrepared* handle);

// index: 0 ear, 1 saf, 2 triple-balance, 3 hoa, 4 saf-binaural, 5 room-binaural.
int32_t mradm_engine_backend(uint32_t index, const MradmEngineBackend** out);
// kind: 0 capabilities, 1 layouts, 2 output formats, 3 input layouts, 4 render support matrix.
int32_t mradm_engine_document(uint32_t kind,
                              const MradmPlatformIslands* platform,
                              MradmEngineText** out,
                              MradmAdmText* document);
void mradm_engine_text_destroy(MradmEngineText* text);

// ── Request resolution ──────────────────────────────────────────────────────────────────────
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
void mradm_engine_result_destroy(MradmEngineResult* result);

// ── Backend preparation and the offline render step ───────────────────────────────────────────

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

// ── File streaming ─────────────────────────────────────────────────────────────────────────────
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

// Optional preimported scene and cached preparation (a Rust backend's or the Apple island's).
// Invalid arguments (including non-UTF-8 or NUL-containing paths) return 1; panics return 6.
int32_t mradm_engine_render_begin(const MradmEngineRenderRequest* request,
                                  const MradmPlatformIslands* platform,
                                  const MradmSceneHandle* preimported,
                                  const MradmRenderPrepared* cached,
                                  MradmEngineRender** output);
// cancelled is the caller's stop token now; texts live until the next call on the handle.
int32_t mradm_engine_render_next(MradmEngineRender* handle, uint32_t cancelled, MradmEngineRenderEvent* event);
int32_t mradm_engine_render_resume(MradmEngineRender* handle, const MradmEngineIslandReply* reply);
// A library's islands: run returns 0 when it ran the island (any failure in reply), nonzero to leave it
// to the caller as an island event. apple (may be NULL) runs the Apple backend's islands 2 and 3.
typedef struct MradmIslandRunner {
    int32_t (*run)(const MradmEngineRenderEvent* request,
                   const MradmIslandContext* context,
                   MradmEngineIslandReply* reply);
    MradmIslandContext driver;
    const MradmAppleIslands* apple;
} MradmIslandRunner;
// Runs the islands runner accepts inside mradm_engine_render_next (copied); NULL returns them again.
int32_t mradm_engine_render_set_islands(MradmEngineRender* handle, const MradmIslandRunner* runner);
int32_t mradm_engine_render_result(MradmEngineRender* handle, MradmEngineRenderResult* output);
// The preparation the render made, or null; destroy with mradm_render_prepared_destroy.
MradmRenderPrepared* mradm_engine_render_take_prepared(MradmEngineRender* handle);
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

// ── The realtime monitor ───────────────────────────────────────────────────────────────────────
typedef struct MradmEngineLiveMonitor MradmEngineLiveMonitor;

// The Scene output over a device it opens: kind is SceneOutputKind (0 system spatial over layout in
// geometry, 1 the stereo hardware device device_id, 2 the null backend). Started and paused.
int32_t mradm_engine_scene_output_open(const MradmEngineSceneStream* stream,
                                       uint32_t kind,
                                       MradmAdmText layout,
                                       uint32_t geometry,
                                       MradmAdmText device_id,
                                       const MradmDeviceIslands* devices,
                                       MradmEngineSceneOutput** output,
                                       uint8_t* message,
                                       size_t* message_len,
                                       uint8_t* context,
                                       size_t* context_len);
// The Scene output over a caller's opened device, taken over in every case.
int32_t mradm_engine_scene_output_create(const MradmEngineSceneStream* stream,
                                         const MradmDeviceTable* device,
                                         uint32_t protect_stereo,
                                         MradmEngineSceneOutput** output,
                                         uint8_t* message,
                                         size_t* message_len,
                                         uint8_t* context,
                                         size_t* context_len);

// The shell's log sink (level: adm::LogLevel) and device controls.
typedef struct MradmEngineLiveMonitorHost {
    void* ctx;
    void (*log)(void*, uint32_t, const uint8_t*, size_t, const uint8_t*, size_t);
    void (*device_flush)(void*);
    void (*device_pause)(void*);
    void (*device_resume)(void*);
} MradmEngineLiveMonitorHost;

// A Rust stream (taken over, with its info) when stream is non-null, else the island.
typedef struct MradmEngineLiveMonitorSource {
    MradmEngineStream* stream;
    MradmEngineStreamInfo info;
    const MradmEngineIslandStream* island;
} MradmEngineLiveMonitorSource;

// The C++ StreamSwitchReceipt of a switch: finish stores each state (1 applied, 2 rejected);
// release drops the reference once the monitor no longer holds it.
typedef struct MradmEngineLiveMonitorReceipt {
    void* ctx;
    void (*finish)(void*, uint32_t);
    void (*release)(void*);
} MradmEngineLiveMonitorReceipt;

// MonitorStatus; state 0 stopped, 1 playing, 2 paused.
typedef struct MradmEngineLiveMonitorStatus {
    uint32_t state;
    uint64_t playhead_frames;
    uint64_t underruns;
    uint64_t buffered_frames;
    float ring_fill;
    uint32_t ended;
    uint32_t failed;
    uint64_t override_revision;
} MradmEngineLiveMonitorStatus;

typedef struct MradmEngineLiveMonitorLevels {
    uint32_t channels;
    float peak[64];
    float rms[64];
    float momentary_lufs;
    float shortterm_lufs;
    float integrated_lufs;
} MradmEngineLiveMonitorLevels;

typedef struct MradmEngineLiveMonitorPull {
    size_t frames;
    uint32_t mark_end;
} MradmEngineLiveMonitorPull;

// Starts the worker; the source is consumed either way. 100: the C++ constructor threw (the
// shell rethrows the message).
int32_t mradm_engine_live_monitor_create(const MradmEngineLiveMonitorSource* source,
                                         uint32_t realtime,
                                         const MradmEngineLiveMonitorHost* host,
                                         MradmEngineLiveMonitor** output,
                                         uint8_t* message,
                                         size_t* message_len,
                                         uint8_t* context,
                                         size_t* context_len);
// Before the device stops; destroy joins the worker after it stopped.
void mradm_engine_live_monitor_begin_shutdown(const MradmEngineLiveMonitor* handle);
void mradm_engine_live_monitor_destroy(MradmEngineLiveMonitor* handle);
void mradm_engine_live_monitor_format(const MradmEngineLiveMonitor* handle,
                                      uint32_t* channels,
                                      uint32_t* sample_rate,
                                      uint32_t* output_stage);
void mradm_engine_live_monitor_play(const MradmEngineLiveMonitor* handle);
void mradm_engine_live_monitor_pause(const MradmEngineLiveMonitor* handle);
void mradm_engine_live_monitor_seek(const MradmEngineLiveMonitor* handle, uint64_t frame);
void mradm_engine_live_monitor_set_loop(const MradmEngineLiveMonitor* handle, uint64_t start, uint64_t end);
int32_t mradm_engine_live_monitor_overrides(const MradmEngineLiveMonitor* handle,
                                            const MradmRenderLiveOverride* values,
                                            size_t values_len,
                                            uint64_t revision,
                                            uint8_t* message,
                                            size_t* message_len,
                                            uint8_t* context,
                                            size_t* context_len);
void mradm_engine_live_monitor_orientation(const MradmEngineLiveMonitor* handle, float yaw, float pitch, float roll);
// The source is consumed; receipt may be null.
void mradm_engine_live_monitor_switch(const MradmEngineLiveMonitor* handle,
                                      const MradmEngineLiveMonitorSource* source,
                                      const MradmEngineLiveMonitorReceipt* receipt);
// The monitor's HpTF processor (mradm_dsp_hptf_live_*), valid until destroy.
const void* mradm_engine_live_monitor_hptf(const MradmEngineLiveMonitor* handle);
MradmRenderRoomControl* mradm_engine_live_monitor_room_control(const MradmEngineLiveMonitor* handle);
void mradm_engine_live_monitor_status(const MradmEngineLiveMonitor* handle, MradmEngineLiveMonitorStatus* out);
void mradm_engine_live_monitor_levels(const MradmEngineLiveMonitor* handle, MradmEngineLiveMonitorLevels* out);
// Device callback: never waits or allocates.
void mradm_engine_live_monitor_pull(const MradmEngineLiveMonitor* handle,
                                    float* output,
                                    size_t output_len,
                                    size_t frames,
                                    MradmEngineLiveMonitorPull* result);

// ── The monitor session ─────────────────────────────────────────────────────────────────────
// The session owns the monitor, its backend options and its device; the shell registers the
// output devices and the Apple islands (either may be NULL). Code 100 from any call means the
// former C++ session threw: the shell rethrows the message.
typedef struct MradmEngineMonitorSession MradmEngineMonitorSession;

typedef struct MradmEngineMonitorSessionHost {
    const MradmDeviceIslands* devices;
    const MradmAppleIslands* apple;
} MradmEngineMonitorSessionHost;

// A session without an engine; null for an invalid host or platform record. The scene handle
// must outlive the session.
MradmEngineMonitorSession* mradm_engine_monitor_session_new(const MradmPlatformIslands* platform,
                                                            const MradmSceneHandle* scene,
                                                            uint32_t sample_rate,
                                                            uint64_t total_frames,
                                                            const MradmEngineMonitorSessionHost* host);
// Appends a log line (level: adm::LogLevel).
void mradm_engine_monitor_session_log(const MradmEngineMonitorSession* handle,
                                      uint32_t level,
                                      const uint8_t* module,
                                      size_t module_len,
                                      const uint8_t* text,
                                      size_t text_len);
// Opens the first stream on the device for device_id and starts it.
int32_t mradm_engine_monitor_session_start(const MradmEngineMonitorSession* handle,
                                           const MradmEngineRenderRequest* request,
                                           uint32_t system_spatial,
                                           const uint8_t* device_id,
                                           size_t device_id_len,
                                           uint8_t* message,
                                           size_t* message_len,
                                           uint8_t* context,
                                           size_t* context_len);
void mradm_engine_monitor_session_destroy(MradmEngineMonitorSession* handle);
void mradm_engine_monitor_session_play(const MradmEngineMonitorSession* handle);
void mradm_engine_monitor_session_pause(const MradmEngineMonitorSession* handle);
int32_t mradm_engine_monitor_session_seek(const MradmEngineMonitorSession* handle,
                                          double seconds,
                                          uint8_t* message,
                                          size_t* message_len,
                                          uint8_t* context,
                                          size_t* context_len);
void mradm_engine_monitor_session_set_loop(const MradmEngineMonitorSession* handle, double start, double end);
int32_t mradm_engine_monitor_session_overrides(const MradmEngineMonitorSession* handle,
                                               const MradmRenderLiveOverride* values,
                                               size_t values_len,
                                               uint64_t revision,
                                               uint8_t* message,
                                               size_t* message_len,
                                               uint8_t* context,
                                               size_t* context_len);
void mradm_engine_monitor_session_orientation(const MradmEngineMonitorSession* handle,
                                              float yaw,
                                              float pitch,
                                              float roll);
int32_t mradm_engine_monitor_session_switch(const MradmEngineMonitorSession* handle,
                                            const MradmEngineRenderRequest* request,
                                            uint32_t system_spatial,
                                            uint8_t* message,
                                            size_t* message_len,
                                            uint8_t* context,
                                            size_t* context_len);
int32_t mradm_engine_monitor_session_output_device(const MradmEngineMonitorSession* handle,
                                                   const uint8_t* device_id,
                                                   size_t device_id_len,
                                                   uint8_t* message,
                                                   size_t* message_len,
                                                   uint8_t* context,
                                                   size_t* context_len);
// settings: a mradm_dsp_hptf_settings_create handle (copied).
int32_t mradm_engine_monitor_session_hptf(const MradmEngineMonitorSession* handle,
                                          const void* settings,
                                          uint8_t* message,
                                          size_t* message_len,
                                          uint8_t* context,
                                          size_t* context_len);
// 0 without an engine (and for the next three).
uint32_t mradm_engine_monitor_session_hptf_snapshot(const MradmEngineMonitorSession* handle, MradmDspHptfSnapshot* out);
uint32_t mradm_engine_monitor_session_status(const MradmEngineMonitorSession* handle,
                                             MradmEngineLiveMonitorStatus* out);
uint32_t mradm_engine_monitor_session_levels(const MradmEngineMonitorSession* handle,
                                             MradmEngineLiveMonitorLevels* out);
size_t mradm_engine_monitor_session_log_count(const MradmEngineMonitorSession* handle);
// The texts stay valid until the next call on this session.
uint32_t mradm_engine_monitor_session_log_entry(
    const MradmEngineMonitorSession* handle, size_t index, uint32_t* level, MradmAdmText* module, MradmAdmText* text);
MradmRenderRoomControl* mradm_engine_monitor_session_room_control(const MradmEngineMonitorSession* handle);

#ifdef __cplusplus
}
#endif
