// The part of the private boundary in src/adm_metadata/adm_ffi.h that production C/C++
// does not use: tests and consistency tools call these mradm-ffi entries (some are also reached
// from Rust inside the workspace). Production keeps only what it calls.
//
// Private ADM metadata boundary: the record view of a render scene (scene_ffi.h) and generated
// metadata buffers, which are borrowed until mradm_adm_buffer_destroy.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_metadata/adm_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmAdmSpeaker {
    uint32_t label;
    float azimuth;
    float elevation;
    uint32_t lfe;
    uint32_t ranges;
    float azimuth_min;
    float azimuth_max;
    float elevation_min;
    float elevation_max;
} MradmAdmSpeaker;
typedef struct MradmAdmBuffer MradmAdmBuffer;
typedef struct MradmAdmOutputChna {
    uint16_t track;
    MradmAdmText uid;
    MradmAdmText format;
    MradmAdmText pack;
} MradmAdmOutputChna;
int32_t mradm_adm_generate(uint32_t kind,
                           MradmAdmText name,
                           const MradmAdmSpeaker* speakers,
                           const MradmAdmText* labels,
                           size_t count,
                           MradmAdmBuffer** output,
                           uint8_t* error,
                           size_t capacity);
int32_t mradm_adm_buffer_view(const MradmAdmBuffer* handle,
                              MradmAdmText* xml,
                              const MradmAdmOutputChna** chna,
                              size_t* length,
                              uint8_t* error,
                              size_t capacity);
void mradm_adm_buffer_destroy(MradmAdmBuffer* handle);

#ifdef __cplusplus
}
#endif
