#include "adm/c_api.h"

#if ADM_API_VERSION_MAJOR != 1
#error "unexpected C ABI major version"
#endif

#if ADM_API_VERSION_MINOR != 34
#error "unexpected C ABI minor version"
#endif

#if ADM_API_VERSION_PATCH != 0
#error "unexpected C ABI patch version"
#endif

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
            ADM_DIRECT_SPEAKERS_ROUTING_MATRIX == 3)
               ? 0
               : 1;
}
