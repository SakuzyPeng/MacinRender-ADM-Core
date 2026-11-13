// Rust mradm-render entries only the C++ tests and the frozen references call (moved from
// src/adm_render_common/routing_ffi.h in PCM consistency 5.6 C): the DirectSpeakers, speaker-label
// and LFE routing rules behind tests/support/adm_scene/render_common_scene.h, the Scene stream
// renderer outside the stream engine (tests/support/live_scene) and the binaural HRTF view
// (tests/support/binaural). Conventions as in routing_ffi.h.
#pragma once
#include "routing_test_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRenderMatrix MradmRenderMatrix;
typedef struct MradmRenderTarget {
    MradmAdmText label;
    uint32_t is_lfe;
} MradmRenderTarget;

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

// A Scene stream renderer outside the stream engine (the test-support live_scene::ILiveSceneRenderer).
typedef struct MradmRenderLive MradmRenderLive;
typedef struct MradmRenderLiveElement {
    uint64_t element_id;
    uint32_t role;
    MradmAdmText speaker_label;
    MradmAdmText object_id;
    uint32_t has_position;
    float position[3];
} MradmRenderLiveElement;
typedef struct MradmRenderLivePlane {
    uint64_t element_id;
    uint32_t has_signal;
    const float* samples;
    size_t samples_len;
} MradmRenderLivePlane;
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

// output is filled even on failure: its log lines precede the error.
int32_t mradm_render_lfe_routing(uint32_t split_power,
                                 MradmAdmText output_layout,
                                 const uint32_t* targets_ptr,
                                 size_t targets_len,
                                 MradmRenderLfeRouting* output,
                                 uint8_t* message,
                                 size_t* message_len);

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
