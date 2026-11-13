// The public C ABI (include/adm/c_api.h) is implemented in Rust (mradm-capi). The C++ left here is
// what an exception needs: the caller's progress callbacks run from these trampolines, so that an
// exception a C++ caller throws from one ends that render with ADM_ERROR_INTERNAL instead of
// unwinding into Rust.
#include <cstdint>

#include "adm/c_api.h"

extern "C" uint32_t mradm_capi_progress_v1(
    adm_progress_cb callback, double fraction, const char* stage, const char* message, void* user) noexcept {
    try {
        callback(fraction, stage, message, user);
        return 0U;
    } catch (...) {
        return 1U;
    }
}

extern "C" uint32_t
mradm_capi_progress_v2(adm_progress_v2_cb callback, const adm_progress_event_v2_t* event, void* user) noexcept {
    try {
        callback(event, user);
        return 0U;
    } catch (...) {
        return 1U;
    }
}
