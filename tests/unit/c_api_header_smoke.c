#include <stddef.h>

#include "adm/c_api.h"

#if ADM_API_VERSION_MAJOR != 1
#error "unexpected C ABI major version"
#endif

#if ADM_API_VERSION_MINOR != 39
#error "unexpected C ABI minor version"
#endif

#if ADM_API_VERSION_PATCH != 0
#error "unexpected C ABI patch version"
#endif

_Static_assert(sizeof(adm_hptf_preamp_mode_t) == sizeof(int), "HpTF preamp mode enum must remain int-sized");
_Static_assert(offsetof(adm_hptf_config_t, struct_size) == 0, "HpTF config must start with struct_size");
_Static_assert(offsetof(adm_hptf_info_t, struct_size) == 0, "HpTF info must start with struct_size");
typedef struct hptf_v1_37_config_layout {
    uint32_t struct_size;
    const char* profile_path;
    int32_t preamp_mode;
    uint32_t reserved_v1_37;
    uint64_t revision;
} hptf_v1_37_config_layout;
_Static_assert(sizeof(adm_hptf_config_t) == sizeof(hptf_v1_37_config_layout), "v1.37 config size is frozen");
_Static_assert(offsetof(adm_hptf_config_t, revision) == offsetof(hptf_v1_37_config_layout, revision),
               "v1.37 revision offset is frozen");
_Static_assert(sizeof(adm_hptf_info_t) == 40, "v1.37 info size is frozen");
_Static_assert(sizeof(adm_hptf_band_type_t) == sizeof(int), "HpTF band enum must remain int-sized");
_Static_assert(offsetof(adm_hptf_band_t, struct_size) == 0, "HpTF band must start with struct_size");
_Static_assert(offsetof(adm_hptf_parameters_t, struct_size) == 0, "HpTF parameters must start with struct_size");
_Static_assert(offsetof(adm_hptf_band_t, reserved_v1_38) == offsetof(adm_hptf_band_t, enabled) + sizeof(int32_t),
               "HpTF band must reserve alignment padding");
_Static_assert(offsetof(adm_hptf_parameters_t, revision) ==
                   offsetof(adm_hptf_parameters_t, reserved_v1_38) + sizeof(uint32_t),
               "HpTF parameters must reserve alignment padding");
_Static_assert(ADM_HPTF_BAND_PEAKING == 0 && ADM_HPTF_BAND_NOTCH == 6, "HpTF band values are stable");
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
