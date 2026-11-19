// The part of the private boundary in src/adm_dsp/scene_math_ffi.h that production C/C++
// does not use: tests and consistency tools call these mradm-ffi entries (some are also reached
// from Rust inside the workspace). Production keeps only what it calls.
//
// POD fields cross the private Rust boundary.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_dsp/scene_math_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

// cppcheck-suppress-begin unusedStructMember
typedef struct MradmSceneCloudPoint {
    float azimuth;
    float elevation;
    float weight;
    uint32_t slot;
} MradmSceneCloudPoint;
typedef struct MradmSceneSpeaker {
    float azimuth;
    float elevation;
    uint32_t is_lfe;
} MradmSceneSpeaker;
// cppcheck-suppress-end unusedStructMember
int mradm_dsp_scene_divergence(
    const float* src, size_t n, uint32_t cartesian, MradmSceneCloudPoint* dst, size_t capacity, size_t* count);
int mradm_dsp_scene_nearest(const float* src,
                            size_t n,
                            uint32_t cartesian,
                            const MradmSceneSpeaker* speakers,
                            size_t speaker_count,
                            size_t* index,
                            float* distance);
int mradm_dsp_scene_channel_lock(const float* src,
                                 size_t n,
                                 uint32_t cartesian,
                                 uint32_t has_max_distance,
                                 float max_distance,
                                 const MradmSceneSpeaker* speakers,
                                 size_t speaker_count,
                                 float* dst,
                                 size_t m,
                                 uint32_t* locked);

#ifdef __cplusplus
}
#endif
