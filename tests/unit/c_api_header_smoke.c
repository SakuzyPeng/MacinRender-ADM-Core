#include "adm/c_api.h"

#if ADM_API_VERSION_MAJOR != 1
#error "unexpected C ABI major version"
#endif

#if ADM_API_VERSION_MINOR != 24
#error "unexpected C ABI minor version"
#endif

#if ADM_API_VERSION_PATCH != 0
#error "unexpected C ABI patch version"
#endif

int main(void) {
    /* cppcheck-suppress knownConditionTrueFalse -- intentional compile-time ABI value guard */
    return (ADM_ERROR_OK == 0 && ADM_ERROR_INTERNAL == 6) ? 0 : 1;
}
