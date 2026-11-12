// Private render-orchestration boundary (mradm-engine): the Rust backends'
// capability records and the discovery JSON documents, the request resolution, the backend
// preparation and offline render step, and the offline render service.
// Backend records are process-lifetime Rust data and are never freed; a document is owned by its
// MradmEngineText handle.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../adm_core/config_ffi.h"
#include "../adm_dsp/dsp_ffi.h"
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

// The islands' platform facts (src/adm_engine/platform_island.h): encoder and MP4 packager
// availability, the Apple backend's process-lifetime record (NULL without it) and its LFE
// self-test (-1 without it), and the system-spatial layouts (process-lifetime records). Any entry
// may be NULL. The engine probes only what each caller needs, as the C++ catalog did.
typedef struct MradmPlatformIslands {
    uint32_t (*apac)(void);
    uint32_t (*iamf)(void);
    uint32_t (*iamf_mp4_packager)(void);
    const MradmEngineBackend* (*apple)(void);
    int32_t (*apple_lfe_routing_ok)(void);
    void (*system_spatial)(const MradmEngineSpatialLayout** layouts, size_t* len);
} MradmPlatformIslands;

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

// The canonical layout id (static text) or the input itself.
int32_t mradm_engine_normalize_layout(const uint8_t* layout, size_t len, MradmAdmText* out);
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

// ── The offline render service ────────────────────────────────────────────────────────────────

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
// The caller's side of an island run inside mradm_engine_render_next: cancelled polls its stop token
// (possibly from another thread), progress receives an event mapped into the render's range, log a
// line the island writes (level: adm::LogLevel).
typedef struct MradmIslandContext {
    void* user;
    uint32_t (*cancelled)(void* user);
    void (*progress)(void* user, const MradmEngineProgress* event);
    void (*log)(void* user, uint32_t level, MradmAdmText module, MradmAdmText message);
} MradmIslandContext;
typedef struct MradmAppleIslands MradmAppleIslands;
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
// An island's progress event mapped into the render's range.
int32_t mradm_engine_render_map_progress(const MradmEngineRender* handle,
                                         const MradmEngineProgress* event,
                                         MradmEngineProgress* output);
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

// ── The monitor's stream opening ───────────────────────────────────────────────────────────────
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
                                  const MradmPlatformIslands* platform,
                                  const MradmSceneHandle* scene,
                                  uint32_t system_spatial,
                                  MradmEngineMonitor** output,
                                  MradmEngineMonitorView* view);
// The opened stream (kind 1, once), or null; destroy with mradm_engine_stream_destroy.
MradmEngineStream* mradm_engine_monitor_take_stream(MradmEngineMonitor* handle);
void mradm_engine_monitor_destroy(MradmEngineMonitor* handle);

// ── The realtime monitor ───────────────────────────────────────────────────────────────────────
typedef struct MradmEngineLiveMonitor MradmEngineLiveMonitor;

// The error buffers of one island call: *_len holds the capacity in and the length out.
typedef struct MradmEngineIslandError {
    uint8_t* message;
    size_t* message_len;
    uint8_t* context;
    size_t* context_len;
} MradmEngineIslandError;

// A C++ IRenderStream. validate and cancel_request may run on any thread, render_output and
// loop_render_reset on the device callback, everything else on the worker. cancel_begin starts a
// fresh stop source for the next seek_cancellable. intermediate_channels is nonzero for an
// output-stage stream. destroy releases the stream.
typedef struct MradmEngineIslandStream {
    void* ctx;
    uint32_t out_channels;
    uint32_t sample_rate;
    uint32_t requires_preroll;
    uint32_t intermediate_channels;
    uint32_t renders_orientation; // renders_orientation_at_output, compared by a backend switch
    int32_t (*process)(void*, float*, size_t, size_t, size_t*, const MradmEngineIslandError*);
    int32_t (*seek)(void*, uint64_t, const MradmEngineIslandError*);
    void (*cancel_begin)(void*);
    void (*cancel_request)(void*);
    int32_t (*seek_cancellable)(void*, uint64_t, const MradmEngineIslandError*);
    int32_t (*validate)(void*, const MradmRenderLiveOverride*, size_t, uint64_t, const MradmEngineIslandError*);
    void (*set_overrides)(void*, const MradmRenderLiveOverride*, size_t, uint64_t);
    void (*set_orientation)(void*, float, float, float);
    int32_t (*produce_intermediate)(void*, float*, size_t, size_t, size_t*, const MradmEngineIslandError*);
    size_t (*render_output)(void*, const float*, size_t, float*, size_t, size_t, float, float, float);
    int32_t (*reposition_source)(void*, uint64_t, const MradmEngineIslandError*);
    void (*loop_render_reset)(void*, uint64_t);
    uint32_t (*render_failed)(void*);
    void (*destroy)(void*);
} MradmEngineIslandStream;

// The RenderPlan fields the Apple backend reads; the values are those of MradmEngineRenderRequest.
typedef struct MradmAppleSettings {
    MradmAdmText input;
    MradmAdmText layout;
    uint32_t routing;
    uint32_t has_matrix;
    MradmDirectMatrixView matrix;
    uint32_t split_power;
    uint32_t speaker_spread;
    uint32_t binaural_spread;
    uint32_t spatial_preset;
    uint32_t speaker_rendering_flags;
    float orientation[3];
} MradmAppleSettings;
typedef struct MradmApplePrepared MradmApplePrepared;
// The Apple backend (src/adm_apple). prepare takes scene (destroyed either way) and returns 0 with
// any failure in reply, *output set on success; render writes output over the window, metrics in
// reply. Reply texts live until the next call on the thread. open_stream returns the stream table
// (holding the preparation) or an error code, 100 for an exception. The context's cancelled and
// progress may be null where unused.
struct MradmAppleIslands {
    int32_t (*prepare)(const MradmAppleSettings* settings,
                       MradmSceneHandle* scene,
                       const MradmIslandContext* context,
                       MradmApplePrepared** output,
                       MradmEngineIslandReply* reply);
    int32_t (*render)(const MradmApplePrepared* prepared,
                      const MradmAppleSettings* settings,
                      MradmAdmText output,
                      uint32_t has_window,
                      uint64_t window_start,
                      uint64_t window_count,
                      const MradmIslandContext* context,
                      MradmEngineIslandReply* reply);
    int32_t (*open_stream)(const MradmApplePrepared* prepared,
                           const MradmAppleSettings* settings,
                           const MradmIslandContext* context,
                           MradmEngineIslandStream* output,
                           const MradmEngineIslandError* error);
    void (*destroy)(MradmApplePrepared* prepared);
};

// A device's pull: fills out (len floats) with up to frames frames and returns how many, on the
// device's audio thread, never blocking or allocating; the device's mark_end may be called inside.
typedef struct MradmDevicePull {
    void* user;
    size_t (*pull)(void* user, float* out, size_t len, size_t frames);
} MradmDevicePull;
// One playback device of an enumeration; the texts live during the call.
typedef struct MradmDeviceEnumeration {
    void* user;
    void (*each)(void* user, MradmAdmText id, MradmAdmText name, uint32_t is_default);
} MradmDeviceEnumeration;
// An opened output device (src/adm_realtime/device_island.h). start begins pulling (the pull record
// is copied) and returns 0 or an error code (100: the device threw, its message the exception's).
// destroy releases the device (after stop).
typedef struct MradmDeviceTable {
    void* ctx;
    int32_t (*start)(void* ctx,
                     uint32_t channels,
                     uint32_t sample_rate,
                     const MradmDevicePull* pull,
                     const MradmEngineIslandError* error);
    void (*stop)(void* ctx);
    void (*flush)(void* ctx);
    void (*pause)(void* ctx);
    void (*resume)(void* ctx);
    void (*mark_end)(void* ctx);
    uint32_t (*realtime)(void* ctx);
    uint32_t (*has_volume)(void* ctx);
    void (*set_volume)(void* ctx, float gain);
    void (*progress)(void* ctx, MradmEngineSceneDeviceProgress* out);
    void (*destroy)(void* ctx);
} MradmDeviceTable;

// The output devices. open kind 0 is the system-spatial sink over layout (geometry: SpeakerGeometry),
// 1 the hardware device device_id ("" = default), 2 miniaudio's null backend; it returns 0 with
// *output set or an error code (unsupported: no system-spatial sink on this platform; 100: the factory
// threw, its message the exception's).
// system_spatial is 1 where the platform has that sink. enumerate calls each once per playback device.
typedef struct MradmDeviceIslands {
    uint32_t system_spatial;
    int32_t (*open)(uint32_t kind,
                    MradmAdmText layout,
                    uint32_t geometry,
                    MradmAdmText device_id,
                    MradmDeviceTable* output,
                    const MradmEngineIslandError* error);
    void (*enumerate)(const MradmDeviceEnumeration* devices);
} MradmDeviceIslands;

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
