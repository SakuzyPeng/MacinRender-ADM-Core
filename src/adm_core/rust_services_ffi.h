// The Rust services the islands call (mradm-ffi `services`): a table Rust hands to the islands (mradm_islands_bind)
// before it first calls one, so the Rust library exports only the C ABI (6.6). Buffers are borrowed for one call;
// handles are destroyed by their matching entry; status values follow adm::ErrorCode; message buffers are UTF-8.
#pragma once
#include "../adm_apple/apple_plan_ffi.h"
#include "../adm_audio/wav_ffi.h"
#include "../adm_render_common/layout_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmRustTestServices MradmRustTestServices;
// clang-format off
typedef struct MradmRustServices {
    // WAVE input, float output and container edits (wav_ffi.h). Paths are UTF-8.
    int32_t (*wav_reader_open)(const uint8_t*, size_t, MradmWavReader**, MradmWavInfo*, uint8_t*, size_t);
    void (*wav_reader_destroy)(MradmWavReader*);
    int32_t (*wav_reader_read)(MradmWavReader*, float*, size_t, uint64_t*, uint8_t*, size_t);
    int32_t (*wav_reader_seek)(MradmWavReader*, uint64_t, uint8_t*, size_t);
    int32_t (*output_writer_create)(const uint8_t*, size_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t,
                                    MradmOutputWriter**, uint8_t*, size_t);
    void (*output_writer_destroy)(MradmOutputWriter*);
    int32_t (*output_writer_write)(MradmOutputWriter*, const float*, size_t, uint8_t*, size_t);
    int32_t (*output_writer_finish)(MradmOutputWriter*, uint8_t*, size_t);
    int32_t (*caf_append_info)(const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*,
                               size_t, uint8_t*, size_t);
    int32_t (*wav_append_bext)(const uint8_t*, size_t, const MradmWavBext*, uint8_t*, size_t);
    // Live gain banks and stereo device protection (gain.h). Lengths count floats; process mode 0 fills gain
    // envelopes, mode 1 multiplies PCM in place.
    int32_t (*dsp_gain_create)(size_t, uint32_t, uint32_t, void**, char*, size_t);
    void (*dsp_gain_destroy)(void*);
    int32_t (*dsp_gain_reset)(void*, char*, size_t);
    int32_t (*dsp_gain_set_targets)(void*, const float*, size_t, char*, size_t);
    int32_t (*dsp_gain_process)(void*, float*, size_t, uint32_t, char*, size_t);
    // The meter (meter.h). Silence or insufficient integrated history is a successful -infinity.
    int32_t (*dsp_meter_create)(uint32_t, uint32_t, uint32_t, const uint32_t*, size_t, void**, char*, size_t);
    void (*dsp_meter_destroy)(void*);
    int32_t (*dsp_meter_add)(void*, const float*, size_t, char*, size_t);
    int32_t (*dsp_meter_reset)(void*, char*, size_t);
    int32_t (*dsp_meter_query)(const void*, uint32_t, uint32_t, double*, char*, size_t);
    // Scene spatial math of the Apple island (scene_math.h).
    int32_t (*dsp_scene_math)(uint32_t, const float*, size_t, float*, size_t);
    int32_t (*dsp_scene_rotation_create)(const float*, size_t, uint32_t, void**);
    void (*dsp_scene_rotation_destroy)(void*);
    int32_t (*dsp_scene_rotation_update)(void*, const float*, size_t);
    int32_t (*dsp_scene_rotation_apply)(const void*, const float*, size_t, float*, size_t, uint32_t);
    // geometry: 0 standard ADM nominal positions, 1 Apple effective positions (layout_ffi.h).
    int32_t (*scene_layouts)(uint32_t, const MradmLayoutEntry**, size_t*, uint8_t*, size_t);
    int32_t (*scene_view)(const MradmSceneHandle*, MradmSceneImportInfo*, MradmAdmView*, uint8_t*, size_t);
    void (*scene_destroy)(MradmSceneHandle*);
    // Bit 1: *gain written; bit 2: *head_locked written; otherwise the channel inherits.
    uint32_t (*render_live_override)(const MradmRenderOverride*, size_t, MradmAdmText, MradmAdmText, float*, uint32_t*);
    float (*render_lfe_gain)(const MradmRenderLfeRouting*, uint32_t, uint32_t);
    // The Apple buses (apple_plan_ffi.h): message and context capacities in *len, lengths out.
    int32_t (*render_apple_prepare)(const MradmRenderAppleRequest*, MradmRenderApplePlan**, MradmRenderApplePlanView*,
                                    uint8_t*, size_t*, uint8_t*, size_t*);
    void (*render_apple_plan_destroy)(MradmRenderApplePlan*);
    const MradmRustTestServices* tests; // only the island tests (tests/support/ffi/island_test_ffi.h)
} MradmRustServices;
// clang-format on

void mradm_islands_bind(const MradmRustServices* services);

#ifdef __cplusplus
}
namespace mradm {
// The table Rust bound before it first called an island; aborts when none is bound.
const MradmRustServices& rust() noexcept;
} // namespace mradm
#endif
