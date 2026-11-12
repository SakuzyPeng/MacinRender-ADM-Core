// The Apple backend's islands (AUSpatialMixer prepare, render and monitor stream), called by the
// engine through MradmIslandRunner::apple and MradmEngineMonitorSessionHost::apple.
#pragma once
#include "../adm_render_common/engine_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

// The process-lifetime table of the macOS build.
const MradmAppleIslands* mradm_apple_islands(void);

#ifdef __cplusplus
}
#endif
