// Platform facts used by the Rust engine: Apple backend capabilities and the LFE self-test,
// and system-spatial layouts from the Apple or Windows sink.
#pragma once
#include "../adm_render_common/engine_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

// The process-lifetime table of this build.
const MradmPlatformIslands* mradm_platform_islands(void);

#ifdef __cplusplus
}
#endif
