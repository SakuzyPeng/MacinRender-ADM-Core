// The islands' platform facts as the engine reads them: encoder and MP4 packager availability
// (the audio island), the Apple backend's capability records and LFE self-test (the Apple island)
// and the system-spatial sink's layouts (the Apple or Windows island).
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
