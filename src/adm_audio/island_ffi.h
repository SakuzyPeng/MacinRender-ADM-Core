// The render islands of the audio library: the encoders, the container metadata and the IAMF MP4
// packager, run by the engine through mradm_engine_render_set_islands.
#pragma once
#include "../adm_render_common/engine_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

// Runs request->island 1 (MP4 packager probe) or 4-9 and returns 0 with any failure in reply; returns 2
// for another island and 1 for a NULL argument. context->cancelled may be called from another thread.
int32_t mradm_audio_island(const MradmEngineRenderEvent* request,
                           const MradmIslandContext* context,
                           MradmEngineIslandReply* reply);

#ifdef __cplusplus
}
#endif
