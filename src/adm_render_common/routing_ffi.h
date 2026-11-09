// Rust mradm-render (PCM consistency 4.5): the DirectSpeakers, speaker-label and LFE routing
// rules, the room control of a file stream (mradm_engine_stream_*, engine_ffi.h) and the Scene
// stream renderers. Inputs are borrowed for the call; views live until the handle's next call
// or destroy. Error buffers: capacity in *len, bytes written out,
// NUL-terminated. Status values follow adm::ErrorCode.
#pragma once
#include "../adm_dsp/room_ffi.h"
#include "../adm_metadata/scene_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRenderMatrix MradmRenderMatrix;
typedef struct MradmRenderPrepared MradmRenderPrepared;

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

// A live edit (`LiveObjectOverride`): the common override and the topology scales.
typedef struct MradmRenderLiveOverride {
    MradmRenderOverride base;
    float extent_scale;
    float extent_width_scale;
    float extent_height_scale;
    float extent_depth_scale;
    float diffuse_scale;
    float divergence_scale;
} MradmRenderLiveOverride;

// Scene stream renderers (live_scene::ILiveSceneRenderer). Roles: 0 object,
// 1 DirectSpeakers, 2 LFE; object_id is the semantic identity's AudioObject id, empty without one. A
// plane's samples are read only with a signal; `trace` is the checkpoint prefix.
typedef struct MradmRenderLive MradmRenderLive;
// live_scene::RendererConfig: selection the RendererSelection value, speaker_spread / binaural_spread the
// C++ enum values, has_sofa a non-empty SOFA path (UTF-8), reference the default geometry when absent;
// room_profile may be null; sink whether the caller publishes diagnostics.
typedef struct MradmEngineSceneRendererRequest {
    uint32_t selection;
    MradmAdmText layout_id;
    uint32_t apple_geometry;
    uint32_t speaker_spread;
    uint32_t binaural_spread;
    uint32_t split_power;
    uint32_t smoothing_frames;
    uint32_t sample_rate;
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
    uint32_t sink;
} MradmEngineSceneRendererRequest;
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
// IRoomControl: concurrent requests/queries; serialize status calls through copying their message.
typedef struct MradmRenderRoomControl MradmRenderRoomControl;
// The binaural HRTF state (4.7): a view of name, rates and sizes, an owned filter-bank handle
// (mradm_dsp_hrtf_filters_destroy) and the HRIRs and directions, valid while the handle lives.
typedef struct MradmBinauralHrtfHandle MradmBinauralHrtfHandle;
typedef struct MradmRenderBinauralHrtfView {
    MradmAdmText name;
    uint32_t sample_rate;
    size_t num_dirs;
    size_t hrir_len;
    size_t fft_size;
    size_t overlap;
    void* filters;
    const float* hrirs;
    size_t hrirs_len;
    const float* directions;
    size_t directions_len;
} MradmRenderBinauralHrtfView;
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

// A backend preparation (mradm_engine_prepare, engine_ffi.h).
void mradm_render_prepared_destroy(MradmRenderPrepared* handle);
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

// Creates the renderer of the configuration: *sample_rate is the rate it renders at, *suffix its checkpoint
// name suffix (valid while the handle lives).
int32_t mradm_engine_scene_renderer_create(const MradmEngineSceneRendererRequest* request,
                                           MradmRenderLive** output,
                                           uint32_t* sample_rate,
                                           MradmAdmText* suffix,
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

// The file renderer's HRTF state outside a render (the engine prepares its own); sofa 0 selects the
// built-in set; path is UTF-8, display and file_name the caller's text of it.
int32_t mradm_render_binaural_hrtf_prepare(uint32_t sofa,
                                           MradmAdmText path,
                                           MradmAdmText display,
                                           MradmAdmText file_name,
                                           uint32_t input_rate,
                                           uint64_t block,
                                           MradmBinauralHrtfHandle** output,
                                           MradmRenderBinauralHrtfView* view,
                                           uint8_t* message,
                                           size_t* message_len,
                                           uint8_t* context,
                                           size_t* context_len);
// The Scene stream renderer's cached state at sample_rate, without HRIRs and directions; sofa 0
// selects the built-in set, path is UTF-8, display and file_name the caller's text of it.
int32_t mradm_render_binaural_hrtf_live(uint32_t sofa,
                                        MradmAdmText path,
                                        MradmAdmText display,
                                        MradmAdmText file_name,
                                        uint32_t sample_rate,
                                        MradmBinauralHrtfHandle** output,
                                        MradmRenderBinauralHrtfView* view,
                                        uint8_t* message,
                                        size_t* message_len,
                                        uint8_t* context,
                                        size_t* context_len);
void mradm_render_binaural_hrtf_destroy(MradmBinauralHrtfHandle* handle);
#ifdef __cplusplus
}
#endif
