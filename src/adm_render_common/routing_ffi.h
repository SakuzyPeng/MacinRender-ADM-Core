// Rust mradm-render (PCM consistency 4.5): the DirectSpeakers, speaker-label and LFE routing
// rules, the offline driver stepped block by block (next, write, commit, finish), and the VBAP,
// HOA and EAR backends with their file streams. Inputs are borrowed for the call; views live until the
// handle's next call or destroy. Error buffers: capacity in *len, bytes written out,
// NUL-terminated. Status values follow adm::ErrorCode.
#pragma once
#include "../adm_dsp/room_ffi.h"
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
    uint64_t window_start;
    uint64_t window_count;
    uint32_t has_meter_window;
    uint64_t meter_start;
    uint64_t meter_count;
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

// mode: 0 standard, 1 D; spread: 0 automatic, 1 none, 2 MDAP; routing: 0 automatic, 1 label,
// 2 position, 3 matrix.
typedef struct MradmRenderTripleBalanceRequest {
    MradmAdmText layout_id;
    uint32_t mode;
    uint32_t spread;
    uint32_t split_power;
    uint32_t standard_geometry;
    uint32_t smoothing_frames;
    uint32_t routing;
    uint32_t has_matrix;
    MradmAdmText input_path;
} MradmRenderTripleBalanceRequest;

// The backend report, also of a rejected preparation.
typedef struct MradmRenderTripleBalanceView {
    MradmAdmText report;
} MradmRenderTripleBalanceView;

// A Triple Balance live edit: the common override and the topology scales it validates.
typedef struct MradmRenderTripleBalanceOverride {
    MradmRenderOverride base;
    float extent_scale;
    float extent_width_scale;
    float extent_height_scale;
    float extent_depth_scale;
    float diffuse_scale;
    float divergence_scale;
} MradmRenderTripleBalanceOverride;

typedef struct MradmRenderTripleBalanceStream MradmRenderTripleBalanceStream;

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
// Scene stream VBAP and Triple Balance renderers (live_scene::ILiveSceneRenderer). Roles: 0 object,
// 1 DirectSpeakers, 2 LFE; object_id is the semantic identity's AudioObject id, empty without one. A
// plane's samples are read only with a signal; `trace` is the checkpoint prefix.
typedef struct MradmRenderLive MradmRenderLive;
typedef struct MradmRenderLiveVbapRequest {
    MradmAdmText layout_id;
    uint32_t geometry;
    uint32_t spread;
    uint32_t split_power;
    uint32_t smoothing_frames;
} MradmRenderLiveVbapRequest;
// geometry 0 standard; spread 0 automatic, 1 none, 2 MDAP.
typedef struct MradmRenderLiveTripleBalanceRequest {
    MradmAdmText layout_id;
    uint32_t geometry;
    uint32_t spread;
    uint32_t has_sofa;
    uint32_t sample_rate;
    uint32_t split_power;
    uint32_t smoothing_frames;
} MradmRenderLiveTripleBalanceRequest;
typedef struct MradmRenderLiveElement {
    uint64_t element_id;
    uint32_t role;
    MradmAdmText speaker_label;
    MradmAdmText object_id;
    uint32_t has_position;
    float position[3];
} MradmRenderLiveElement;
typedef struct MradmRenderLiveState {
    uint64_t valid_fields;
    uint32_t active;
    float linear_gain;
    float position[3];
    float width;
    float height;
    float depth;
    float diffuse;
    float divergence;
    uint32_t channel_lock;
    uint32_t screen_reference;
    uint32_t head_locked;
    float divergence_azimuth_range;
    float divergence_position_range;
    uint32_t has_max_distance;
    float max_distance;
} MradmRenderLiveState;
typedef struct MradmRenderLivePlane {
    uint64_t element_id;
    uint32_t has_signal;
    const float* samples;
    size_t samples_len;
} MradmRenderLivePlane;
typedef struct MradmRenderLiveEntry {
    uint64_t element_id;
    MradmRenderLiveState state;
} MradmRenderLiveEntry;
typedef struct MradmRenderLiveUpdate {
    uint64_t element_id;
    uint32_t offset;
    uint32_t ramp;
    uint32_t jump;
    uint64_t changed;
    uint64_t cleared;
    MradmRenderLiveState state;
} MradmRenderLiveUpdate;
typedef struct MradmRenderLiveFrame {
    uint64_t generation;
    uint32_t duration;
    const MradmRenderLivePlane* planes;
    size_t planes_len;
    const MradmRenderLiveEntry* initial;
    size_t initial_len;
    const MradmRenderLiveUpdate* updates;
    size_t updates_len;
    MradmAdmText trace;
} MradmRenderLiveFrame;
typedef struct MradmRenderLiveDiagnostic {
    uint32_t code;
    uint64_t element_id;
    uint64_t field;
    MradmAdmText message;
} MradmRenderLiveDiagnostic;
// Room renderer preparation (PCM consistency 4.6 D): spread 0 automatic, 1 none, 2 cloud, 3 SAF
// spreader; routing 0 automatic, 1 label, 2 position, 3 matrix.
typedef struct MradmRenderRoomRequest {
    MradmAdmText layout_id;
    uint32_t sample_rate;
    uint32_t spread;
    uint32_t routing;
    uint32_t has_matrix;
    uint32_t default_interp_ms;
    uint32_t has_reference;
    MradmRoomScreenGeometry reference;
    uint32_t has_reproduction;
    MradmRoomScreenGeometry reproduction;
} MradmRenderRoomRequest;
// A RoomConfig: a room profile handle, the UTF-8 personal HRIR path (empty for none), the scale
// and the quality (0 exact, 1 realtime).
typedef struct MradmRenderRoomConfig {
    const void* profile;
    MradmAdmText hrir;
    double metres;
    uint32_t quality;
} MradmRenderRoomConfig;
// A RoomStatus; message lives until the next status call on the control handle.
typedef struct MradmRenderRoomStatus {
    uint32_t state;
    uint32_t constrained_sources;
    uint64_t requested_revision;
    uint64_t applied_revision;
    uint64_t transition_sample;
    uint64_t material_frames;
    uint64_t tail_frames;
    uint64_t buffer_frames;
    uint64_t material_filter_guard_frames;
    uint32_t quality;
    uint32_t reflection_update_frames;
    MradmAdmText message;
} MradmRenderRoomStatus;
// IRoomControl, independent of the stream or Scene renderer that issued it.
typedef struct MradmRenderRoomControl MradmRenderRoomControl;
// The room file stream (PCM consistency 4.6 E). Serialize calls; destroy after all calls return.
typedef struct MradmRenderRoomStream MradmRenderRoomStream;
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

// Driver errors set *location: 0 none, 1 path=<input>, 2 input=<input>, 3 meter, 4 HOA DSP,
// 5 Triple Balance DSP, 6 room-binaural.
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
int32_t mradm_render_triple_balance_prepare(const MradmSceneHandle* scene,
                                            const MradmRenderTripleBalanceRequest* request,
                                            MradmRenderPrepared** output,
                                            MradmRenderPreparedView* view,
                                            MradmRenderTripleBalanceView* triple_balance);
// Standard-mode file stream. Serialize process/seek/overrides; validate/requires_preroll may run concurrently.
// Step seeks through begin/step/finish, checking cancellation while pending. Destroy after all calls return.
int32_t mradm_render_triple_balance_stream_open(const MradmRenderPrepared* prepared,
                                                const uint8_t* path,
                                                size_t path_len,
                                                uint32_t default_interp_ms,
                                                MradmRenderTripleBalanceStream** output,
                                                uint32_t* location,
                                                uint8_t* message,
                                                size_t* message_len);
int32_t mradm_render_triple_balance_stream_process(MradmRenderTripleBalanceStream* handle,
                                                   float* output,
                                                   size_t output_len,
                                                   size_t frames,
                                                   size_t* produced,
                                                   uint32_t* location,
                                                   uint8_t* message,
                                                   size_t* message_len);
int32_t mradm_render_triple_balance_stream_seek_begin(MradmRenderTripleBalanceStream* handle,
                                                      uint64_t frame,
                                                      uint32_t* pending,
                                                      uint32_t* location,
                                                      uint8_t* message,
                                                      size_t* message_len);
int32_t mradm_render_triple_balance_stream_seek_step(MradmRenderTripleBalanceStream* handle,
                                                     uint32_t* pending,
                                                     uint32_t* location,
                                                     uint8_t* message,
                                                     size_t* message_len);
int32_t mradm_render_triple_balance_stream_seek_finish(MradmRenderTripleBalanceStream* handle,
                                                       uint32_t* location,
                                                       uint8_t* message,
                                                       size_t* message_len);
int32_t mradm_render_triple_balance_stream_validate(const MradmRenderTripleBalanceStream* handle,
                                                    const MradmRenderTripleBalanceOverride* values,
                                                    size_t values_len,
                                                    uint8_t* message,
                                                    size_t* message_len,
                                                    uint8_t* context,
                                                    size_t* context_len);
int32_t mradm_render_triple_balance_stream_overrides(MradmRenderTripleBalanceStream* handle,
                                                     const MradmRenderTripleBalanceOverride* values,
                                                     size_t values_len);
// 1 when size tracks make a seek render from an earlier block.
uint32_t mradm_render_triple_balance_stream_requires_preroll(const MradmRenderTripleBalanceStream* handle);
void mradm_render_triple_balance_stream_destroy(MradmRenderTripleBalanceStream* handle);
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

int32_t mradm_render_room_prepare(const MradmSceneHandle* scene,
                                  const MradmRenderRoomRequest* request,
                                  MradmRenderPrepared** output,
                                  MradmRenderPreparedView* view);

int32_t mradm_room_control_request(const MradmRenderRoomControl* handle,
                                   const MradmRenderRoomConfig* config,
                                   uint64_t* revision,
                                   uint8_t* message,
                                   size_t* message_len,
                                   uint8_t* context,
                                   size_t* context_len);
void mradm_room_control_status(MradmRenderRoomControl* handle, MradmRenderRoomStatus* out);
void mradm_room_control_source(const MradmRenderRoomControl* handle, uint64_t id, MradmRoomSourceInfo* out);
void mradm_room_control_destroy(MradmRenderRoomControl* handle);

// Opens the stream of a room preparation and its room set, seeked to 0; pose is yaw, pitch, roll.
int32_t mradm_render_room_stream_open(const MradmRenderPrepared* prepared,
                                      const void* room,
                                      const uint8_t* path,
                                      size_t path_len,
                                      const MradmRenderRoomConfig* config,
                                      const float* pose,
                                      MradmRenderRoomStream** output,
                                      uint32_t* location,
                                      uint8_t* message,
                                      size_t* message_len);
int32_t mradm_render_room_stream_process(MradmRenderRoomStream* handle,
                                         float* output,
                                         size_t output_len,
                                         size_t frames,
                                         size_t* produced,
                                         uint32_t* location,
                                         uint8_t* message,
                                         size_t* message_len);
// *pending (set unless the call fails) is whether a history block remains to be stepped.
int32_t mradm_render_room_stream_seek_begin(MradmRenderRoomStream* handle,
                                            uint64_t frame,
                                            uint32_t* pending,
                                            uint32_t* location,
                                            uint8_t* message,
                                            size_t* message_len);
int32_t mradm_render_room_stream_seek_step(
    MradmRenderRoomStream* handle, uint32_t* pending, uint32_t* location, uint8_t* message, size_t* message_len);
int32_t mradm_render_room_stream_validate(MradmRenderRoomStream* handle,
                                          const MradmRenderTripleBalanceOverride* values,
                                          size_t values_len,
                                          uint8_t* message,
                                          size_t* message_len);
// Latches a rejected update or orientation for the next process.
int32_t mradm_render_room_stream_overrides(MradmRenderRoomStream* handle,
                                           const MradmRenderTripleBalanceOverride* values,
                                           size_t values_len);
void mradm_render_room_stream_pose(MradmRenderRoomStream* handle, float yaw, float pitch, float roll);
uint64_t mradm_render_room_stream_tail(const MradmRenderRoomStream* handle);
// A new control handle; null for a null stream.
MradmRenderRoomControl* mradm_render_room_stream_control(const MradmRenderRoomStream* handle);
void mradm_render_room_stream_destroy(MradmRenderRoomStream* handle);

int32_t mradm_render_live_vbap_create(const MradmRenderLiveVbapRequest* request,
                                      MradmRenderLive** output,
                                      uint8_t* message,
                                      size_t* message_len);
int32_t mradm_render_live_triple_balance_create(const MradmRenderLiveTripleBalanceRequest* request,
                                                MradmRenderLive** output,
                                                uint8_t* message,
                                                size_t* message_len);
// The Scene stream room renderer over a room set: spread is binaural_spread_mode != none, sink
// whether the caller publishes diagnostics.
int32_t mradm_render_live_room_create(const void* room,
                                      const MradmRenderRoomConfig* config,
                                      uint32_t spread,
                                      const MradmRoomScreens* screens,
                                      uint32_t sink,
                                      MradmRenderLive** output,
                                      uint8_t* message,
                                      size_t* message_len,
                                      uint8_t* context,
                                      size_t* context_len);
// The room control of a Scene stream room renderer; null for another renderer.
MradmRenderRoomControl* mradm_render_live_room_control(const MradmRenderLive* handle);
void mradm_render_live_destroy(MradmRenderLive* handle);
uint32_t mradm_render_live_channels(const MradmRenderLive* handle);
int32_t mradm_render_live_configure(MradmRenderLive* handle,
                                    uint64_t generation,
                                    const MradmRenderLiveElement* elements,
                                    size_t elements_len,
                                    const MradmRenderLiveDiagnostic** diagnostics,
                                    size_t* diagnostics_len,
                                    uint8_t* message,
                                    size_t* message_len,
                                    uint8_t* context,
                                    size_t* context_len);
void mradm_render_live_reset(MradmRenderLive* handle);
// Latches a rejected orientation (room) for the next render.
void mradm_render_live_pose(MradmRenderLive* handle, float yaw, float pitch, float roll);
// Ends the input; *tail is the frames the renderer appends (0 without a room).
int32_t mradm_render_live_end_input(MradmRenderLive* handle,
                                    uint64_t* tail,
                                    uint8_t* message,
                                    size_t* message_len,
                                    uint8_t* context,
                                    size_t* context_len);
uint64_t mradm_render_live_tail(const MradmRenderLive* handle);
// *diagnostics lists the frame's warnings until the next call on the handle, also after a failure:
// room publishes those raised before it, VBAP and Triple Balance none, and leave the output unchanged.
int32_t mradm_render_live_render(MradmRenderLive* handle,
                                 const MradmRenderLiveFrame* frame,
                                 float* output,
                                 size_t output_len,
                                 const MradmRenderLiveDiagnostic** diagnostics,
                                 size_t* diagnostics_len,
                                 uint8_t* message,
                                 size_t* message_len,
                                 uint8_t* context,
                                 size_t* context_len);

#ifdef __cplusplus
}
#endif
