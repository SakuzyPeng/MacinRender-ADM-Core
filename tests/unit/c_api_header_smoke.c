#include <stddef.h>

#include "adm/c_api.h"

#if ADM_API_VERSION_MAJOR != 1
#error "unexpected C ABI major version"
#endif

#if ADM_API_VERSION_MINOR != 36
#error "unexpected C ABI minor version"
#endif

#if ADM_API_VERSION_PATCH != 0
#error "unexpected C ABI patch version"
#endif

_Static_assert(sizeof(adm_scene_element_role_t) == sizeof(int), "Scene role enum must remain int-sized");
_Static_assert(sizeof(adm_scene_submit_status_t) == sizeof(int), "Scene submit enum must remain int-sized");
_Static_assert(sizeof(adm_scene_stream_state_t) == sizeof(int), "Scene state enum must remain int-sized");
_Static_assert(offsetof(adm_scene_stream_config_t, struct_size) == 0, "Scene config must start with struct_size");
_Static_assert(offsetof(adm_scene_renderer_config_t, struct_size) == 0,
               "Scene renderer config must start with struct_size");
_Static_assert(offsetof(adm_scene_semantic_entity_t, struct_size) == 0,
               "Scene semantic entity must start with struct_size");
_Static_assert(offsetof(adm_scene_semantic_identity_t, struct_size) == 0,
               "Scene semantic identity must start with struct_size");
_Static_assert(offsetof(adm_scene_element_descriptor_t, struct_size) == 0,
               "Scene descriptor must start with struct_size");
_Static_assert(offsetof(adm_scene_object_state_t, struct_size) == 0, "Scene state must start with struct_size");
_Static_assert(offsetof(adm_scene_pcm_plane_t, struct_size) == 0, "Scene PCM must start with struct_size");
_Static_assert(offsetof(adm_scene_frame_t, struct_size) == 0, "Scene frame must start with struct_size");
_Static_assert(offsetof(adm_scene_pull_result_t, struct_size) == 0, "Scene pull result must start with struct_size");
_Static_assert(offsetof(adm_scene_stream_status_t, struct_size) == 0, "Scene status must start with struct_size");
_Static_assert(offsetof(adm_scene_diagnostic_t, struct_size) == 0, "Scene diagnostic must start with struct_size");

int main(void) {
    // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
    return (ADM_ERROR_OK == 0 && ADM_ERROR_INTERNAL == 6 && ADM_LFE_ROUTING_DIRECT == 0 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_LFE_ROUTING_SPLIT_POWER == 1 && ADM_SPEAKER_GEOMETRY_STANDARD == 0 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_SPEAKER_GEOMETRY_APPLE == 1 && ADM_DIRECT_SPEAKERS_ROUTING_AUTOMATIC == 0 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_DIRECT_SPEAKERS_ROUTING_LABEL == 1 && ADM_DIRECT_SPEAKERS_ROUTING_POSITION == 2 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_DIRECT_SPEAKERS_ROUTING_MATRIX == 3 && ADM_SCENE_ELEMENT_OBJECT == 0 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_SCENE_ELEMENT_DIRECT_SPEAKER == 1 && ADM_SCENE_ELEMENT_LFE == 2 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_SCENE_SUBMIT_ACCEPTED == 0 && ADM_SCENE_SUBMIT_CLOSED == 3 &&
            // cppcheck-suppress knownConditionTrueFalse; intentional compile-time ABI value guard.
            ADM_SCENE_SAMPLE_F32 == 0 && ADM_SCENE_STATE_POSITION == (UINT64_C(1) << 2) &&
            ADM_SCENE_STATE_DIVERGENCE_RANGE == (UINT64_C(1) << 9) &&
            ADM_SCENE_STATE_CHANNEL_LOCK_MAX_DISTANCE == (UINT64_C(1) << 10))
               ? 0
               : 1;
}
