// Rust mradm-render `apple`: the Apple island's AUSpatialMixer buses from the policy-applied scene.
// Objects are point sources (divergence and extent clouds on parallel buses); DirectSpeakers are beds,
// host-side direct buses or LFE bypass buses. Event azimuth is positive to the right (AudioUnit convention).
// Error buffers: capacity in *len, bytes written out, NUL-terminated; status values follow adm::ErrorCode.
#pragma once
#include "../adm_metadata/scene_ffi.h"
#include "../adm_render_common/routing_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRenderApplePlan MradmRenderApplePlan;
// routing: adm::DirectSpeakersRoutingMode. log receives the "apple" lines in order (level 1 info,
// 2 warning), also before an error; its text is valid for the call.
typedef struct MradmRenderAppleRequest {
    const MradmSceneHandle* scene;
    MradmAdmText layout_id;
    uint32_t binaural;
    uint32_t routing;
    uint32_t has_matrix;
    const MradmRenderMatrixRoute* matrix;
    size_t matrix_len;
    uint32_t lfe_split_power;
    uint32_t speaker_spread_none;
    uint32_t binaural_spread_cloud;
    MradmAdmText input_path;
    void* log_context;
    void (*log)(void* context, uint32_t level, MradmAdmText text);
} MradmRenderAppleRequest;
typedef struct MradmRenderAppleOutput {
    size_t output;
    float gain;
} MradmRenderAppleOutput;
// lfe_target: 0 none, 1 LFE1, 2 LFE2.
typedef struct MradmRenderAppleEvent {
    uint64_t start;
    uint64_t end;
    float azimuth;
    float elevation;
    float distance;
    float gain;
    uint32_t lfe_target;
    uint32_t head_locked;
    const MradmRenderAppleOutput* outputs;
    size_t outputs_len;
} MradmRenderAppleEvent;
// mode: 0 point source, 1 ambience bed, 2 bypass.
typedef struct MradmRenderAppleBus {
    uint32_t channel;
    uint32_t mode;
    uint32_t is_lfe;
    uint32_t is_direct;
    MradmAdmText object_id;
    MradmAdmText label_key;
    const MradmRenderAppleEvent* events;
    size_t events_len;
} MradmRenderAppleBus;
// routing: the resolved DirectSpeakers mode (1 label, 2 position, 3 matrix); the input format is
// the scene's.
typedef struct MradmRenderApplePlanView {
    uint32_t input_channels;
    uint32_t sample_rate;
    uint64_t frames;
    uint32_t routing;
    uint32_t applies_to_22_2;
    uint32_t split_power;
    uint32_t has_lfe1;
    uint32_t has_lfe2;
    const MradmRenderAppleBus* spatial;
    size_t spatial_len;
    const MradmRenderAppleBus* direct;
    size_t direct_len;
    const MradmRenderAppleBus* lfe;
    size_t lfe_len;
} MradmRenderApplePlanView;
// cppcheck-suppress-end unusedStructMember
// On success *output owns the records *view points into, until mradm_render_apple_plan_destroy.
int32_t mradm_render_apple_prepare(const MradmRenderAppleRequest* request,
                                   MradmRenderApplePlan** output,
                                   MradmRenderApplePlanView* view,
                                   uint8_t* message,
                                   size_t* message_len,
                                   uint8_t* context,
                                   size_t* context_len);
void mradm_render_apple_plan_destroy(MradmRenderApplePlan* plan);
#ifdef __cplusplus
}
#endif
