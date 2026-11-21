// The render scene of the Rust services (rust_services_ffi.h, mradm-scene): an opaque handle the Apple island
// passes back to Rust and releases with scene_destroy.
#pragma once
#include "adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmSceneHandle MradmSceneHandle;

#ifdef __cplusplus
}
#endif
