// The part of the private boundary in src/adm_render_common/routing_ffi.h that production C/C++
// does not use: tests and consistency tools call these mradm-ffi entries (some are also reached
// from Rust inside the workspace). Production keeps only what it calls.
//
// Rust mradm-render: the live-override and LFE gain rules, the room control
// of a file stream (mradm_engine_stream_*, engine_ffi.h) and the Scene stream engine. The routing
// rules, the Scene stream renderer outside the engine and the HRTF view only the tests call are in
// tests/support/render_test_ffi.h. Inputs are borrowed for the call; views live until the
// handle's next call or destroy. Error buffers: capacity in *len, bytes written out, NUL-terminated. Status values
// follow adm::ErrorCode.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_render_common/routing_ffi.h"
#include "dsp_test_ffi.h"
#include "room_test_ffi.h"
#include "scene_test_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

// The Scene stream renderer request and element state (the stream engine; the test-support renderer
// outside it is render_test_ffi.h). Roles: 0 object, 1 DirectSpeakers, 2 LFE.
// live_scene::RendererConfig: selection the RendererSelection value, speaker_spread / binaural_spread the
// C++ enum values, has_sofa a non-empty SOFA path (UTF-8), reference the default geometry when absent;
// room_profile may be null; sink whether the caller publishes diagnostics.
// cppcheck-suppress-begin unusedStructMember
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
// The Scene stream engine (mradm-engine scene::stream). The configuration
// leaves out the renderer; manual_clock selects the replay clock starting at clock_ns; the *_bytes
// are the C++ sizeof of live_scene::Frame, PcmPlane, StateEntry and MetadataUpdate.
typedef struct MradmEngineSceneConfig {
    uint32_t output_sample_rate;
    uint32_t input_queue_samples;
    uint64_t input_queue_bytes;
    uint32_t output_ring_frames;
    uint32_t startup_watermark_frames;
    uint32_t manual_clock;
    int64_t clock_ns;
    size_t frame_bytes;
    size_t plane_bytes;
    size_t entry_bytes;
    size_t update_bytes;
} MradmEngineSceneConfig;
typedef struct MradmEngineSceneEntity {
    MradmAdmText id;
    MradmAdmText name;
} MradmEngineSceneEntity;
// live_scene::SemanticIdentity; present is 0 for none.
typedef struct MradmEngineSceneIdentity {
    uint32_t present;
    MradmAdmText object_id;
    MradmAdmText object_name;
    MradmAdmText track_uid;
    uint32_t has_importance;
    int32_t importance;
    uint32_t has_dialogue_id;
    int32_t dialogue_id;
    const MradmEngineSceneEntity* contents;
    size_t contents_len;
    const MradmEngineSceneEntity* programmes;
    size_t programmes_len;
} MradmEngineSceneIdentity;
// live_scene::ElementDescriptor; role is the ElementRole value.
typedef struct MradmEngineSceneElement {
    uint64_t element_id;
    uint32_t role;
    MradmAdmText speaker_label;
    uint32_t has_position;
    float position[3];
    uint64_t flags;
    MradmEngineSceneIdentity identity;
} MradmEngineSceneElement;
// ScenePcmPlaneView: samples[k * stride] is sample k.
typedef struct MradmEngineScenePlane {
    uint64_t element_id;
    const float* samples;
    uint32_t sample_count;
    uint32_t stride;
    uint32_t has_signal;
} MradmEngineScenePlane;
typedef struct MradmEngineSceneFrame {
    uint64_t epoch;
    uint64_t generation;
    int64_t start;
    uint32_t duration;
    uint32_t flags;
    const MradmEngineScenePlane* planes;
    size_t planes_len;
    const MradmRenderLiveEntry* initial;
    size_t initial_len;
    const MradmRenderLiveUpdate* updates;
    size_t updates_len;
} MradmEngineSceneFrame;
typedef struct MradmEngineScenePull {
    uint32_t flags;
    uint64_t epoch;
    uint64_t first_media_frame;
    uint32_t media_frames;
    uint32_t requested_frames;
} MradmEngineScenePull;
// SceneStreamStatus; state is the SceneStreamState value.
typedef struct MradmEngineSceneStatus {
    uint32_t state;
    uint64_t epoch;
    uint64_t generation;
    uint64_t queued_input_samples;
    uint64_t queued_input_bytes;
    uint64_t buffered_output_frames;
    uint64_t media_frames_pulled;
    uint64_t underruns;
    uint64_t semantic_degradations;
    uint64_t semantic_policy_revision;
    float ring_fill;
    uint32_t ended;
    uint32_t failed;
    uint32_t production_complete;
} MradmEngineSceneStatus;
typedef struct MradmEngineSceneOutput MradmEngineSceneOutput;
// SceneDeviceStatus; state is the SceneOutputState value.
typedef struct MradmEngineSceneOutputStatus {
    uint32_t state;
    uint64_t epoch;
    uint64_t consumed_frames;
    uint64_t presented_frames;
    uint64_t queued_frames;
    uint64_t underruns;
    uint32_t media_clock;
    uint32_t recovering;
    uint32_t failed;
    uint32_t ended;
} MradmEngineSceneOutputStatus;
// live_scene::Diagnostic without its text; message_len is the full text length.
typedef struct MradmEngineSceneDiagnostic {
    uint32_t level;
    uint32_t code;
    uint64_t epoch;
    uint64_t generation;
    uint64_t element;
    uint64_t field;
    size_t message_len;
} MradmEngineSceneDiagnostic;
// cppcheck-suppress-end unusedStructMember
typedef struct MradmEngineSceneStream MradmEngineSceneStream;
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

// The Scene stream engine; every error call writes code, message and context. The handle's calls
// may run on any thread, mradm_engine_scene_pull on the audio thread (no blocking, no allocation).
int32_t mradm_engine_scene_create(const MradmEngineSceneConfig* config,
                                  const MradmEngineSceneRendererRequest* renderer,
                                  MradmEngineSceneStream** output,
                                  uint8_t* message,
                                  size_t* message_len,
                                  uint8_t* context,
                                  size_t* context_len);
void mradm_engine_scene_destroy(MradmEngineSceneStream* handle);
void mradm_engine_scene_format(const MradmEngineSceneStream* handle,
                               uint32_t* output_rate,
                               uint32_t* channels,
                               uint32_t* input_rate);
int32_t mradm_engine_scene_begin_epoch(const MradmEngineSceneStream* handle,
                                       uint64_t epoch,
                                       int64_t target,
                                       uint8_t* message,
                                       size_t* message_len,
                                       uint8_t* context,
                                       size_t* context_len);
int32_t mradm_engine_scene_configure(const MradmEngineSceneStream* handle,
                                     uint64_t epoch,
                                     uint64_t generation,
                                     const MradmEngineSceneElement* elements,
                                     size_t elements_len,
                                     uint8_t* message,
                                     size_t* message_len,
                                     uint8_t* context,
                                     size_t* context_len);
// *status is the SceneSubmitStatus value when the call succeeds.
int32_t mradm_engine_scene_submit(const MradmEngineSceneStream* handle,
                                  const MradmEngineSceneFrame* frame,
                                  int64_t timeout_ms,
                                  uint32_t* status,
                                  uint8_t* message,
                                  size_t* message_len,
                                  uint8_t* context,
                                  size_t* context_len);
int32_t mradm_engine_scene_signal_end(const MradmEngineSceneStream* handle,
                                      uint64_t epoch,
                                      int64_t end,
                                      uint8_t* message,
                                      size_t* message_len,
                                      uint8_t* context,
                                      size_t* context_len);
int32_t mradm_engine_scene_switch_backend(const MradmEngineSceneStream* handle,
                                          const MradmEngineSceneRendererRequest* renderer,
                                          uint8_t* message,
                                          size_t* message_len,
                                          uint8_t* context,
                                          size_t* context_len);
int32_t mradm_engine_scene_wait_idle(const MradmEngineSceneStream* handle,
                                     int64_t timeout_ms,
                                     uint8_t* message,
                                     size_t* message_len,
                                     uint8_t* context,
                                     size_t* context_len);
void mradm_engine_scene_orientation(const MradmEngineSceneStream* handle, float yaw, float pitch, float roll);
// The current backend's room control; null without a room renderer.
MradmRenderRoomControl* mradm_engine_scene_room_control(const MradmEngineSceneStream* handle);
int32_t mradm_engine_scene_policy(const MradmEngineSceneStream* handle,
                                  const uint8_t* json,
                                  size_t json_len,
                                  uint64_t revision,
                                  uint8_t* message,
                                  size_t* message_len,
                                  uint8_t* context,
                                  size_t* context_len);
void mradm_engine_scene_pull(const MradmEngineSceneStream* handle,
                             float* output,
                             size_t output_len,
                             uint32_t frames,
                             MradmEngineScenePull* result);
// attach 1 attaches (returns 1 when it took effect), 0 detaches.
uint32_t mradm_engine_scene_attach(const MradmEngineSceneStream* handle, uint32_t attach);
uint32_t mradm_engine_scene_attached(const MradmEngineSceneStream* handle);
void mradm_engine_scene_status(const MradmEngineSceneStream* handle, MradmEngineSceneStatus* out);
size_t mradm_engine_scene_diagnostic_count(const MradmEngineSceneStream* handle);
// 0 when diagnostic index exists; its text is copied up to capacity bytes.
int32_t mradm_engine_scene_diagnostic(const MradmEngineSceneStream* handle,
                                      size_t index,
                                      MradmEngineSceneDiagnostic* out,
                                      uint8_t* message,
                                      size_t capacity);
void mradm_engine_scene_set_clock(const MradmEngineSceneStream* handle, int64_t ns);

// The Scene output session and its device (opened with mradm_engine_scene_output_open or
// mradm_engine_scene_output_create in engine_ffi.h). The control calls are serialised; pause parks
// the session (waits until no pull is inside) and pauses the device. Errors write code, message
// and context.
void mradm_engine_scene_output_destroy(MradmEngineSceneOutput* handle);
void mradm_engine_scene_output_pause(const MradmEngineSceneOutput* handle);
void mradm_engine_scene_output_play(const MradmEngineSceneOutput* handle);
// A stale epoch is rejected before the device queue is disturbed; otherwise park, flush, reset.
int32_t mradm_engine_scene_output_begin_epoch(const MradmEngineSceneOutput* handle,
                                              uint64_t epoch,
                                              int64_t target,
                                              uint8_t* message,
                                              size_t* message_len,
                                              uint8_t* context,
                                              size_t* context_len);
int32_t mradm_engine_scene_output_set_volume(const MradmEngineSceneOutput* handle,
                                             float gain,
                                             uint8_t* message,
                                             size_t* message_len,
                                             uint8_t* context,
                                             size_t* context_len);
// A null coefficients pointer publishes the bypass.
int32_t mradm_engine_scene_output_set_hptf(const MradmEngineSceneOutput* handle,
                                           const MradmDspHptfCoefficients* coefficients,
                                           uint64_t revision,
                                           uint8_t* message,
                                           size_t* message_len,
                                           uint8_t* context,
                                           size_t* context_len);
// settings is a mradm_dsp_hptf_settings_create handle, designed for the stream's rate.
int32_t mradm_engine_scene_output_set_hptf_settings(const MradmEngineSceneOutput* handle,
                                                    const void* settings,
                                                    uint8_t* message,
                                                    size_t* message_len,
                                                    uint8_t* context,
                                                    size_t* context_len);
void mradm_engine_scene_output_hptf_snapshot(const MradmEngineSceneOutput* handle, MradmDspHptfSnapshot* out);
void mradm_engine_scene_output_status(const MradmEngineSceneOutput* handle, MradmEngineSceneOutputStatus* out);

#ifdef __cplusplus
}
#endif
