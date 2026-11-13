// The DirectSpeakers matrix records of Rust mradm-scene that the engine records carry. The matrix
// and screen configuration entries only tests call are in tests/support/ffi/config_test_ffi.h.
#pragma once
#include "../adm_metadata/adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDirectMatrixTarget {
    MradmAdmText label;
    float weight;
    float gain;
} MradmDirectMatrixTarget;

typedef struct MradmDirectMatrixRoute {
    MradmAdmText source_label;
    uint32_t mute;
    const MradmDirectMatrixTarget* targets;
    size_t targets_len;
} MradmDirectMatrixRoute;

typedef struct MradmDirectMatrixView {
    MradmAdmText output_layout;
    const MradmDirectMatrixRoute* routes;
    size_t routes_len;
} MradmDirectMatrixView;
// cppcheck-suppress-end unusedStructMember

#ifdef __cplusplus
}
#endif
