#pragma once
#include <stddef.h>
#include <stdint.h>
// POD fields cross the private Rust boundary.
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
int mradm_dsp_scene_math(uint32_t op, const float* src, size_t n, float* dst, size_t m);
int mradm_dsp_scene_cloud(const float* src,
                          size_t n,
                          uint32_t cartesian,
                          uint32_t binaural,
                          MradmSceneCloudPoint* dst,
                          size_t capacity,
                          size_t* count,
                          float* trace,
                          size_t trace_len);
int mradm_dsp_scene_divergence(
    const float* src, size_t n, uint32_t cartesian, MradmSceneCloudPoint* dst, size_t capacity, size_t* count);
int mradm_dsp_scene_nearest(const float* src,
                            size_t n,
                            uint32_t cartesian,
                            const MradmSceneSpeaker* speakers,
                            size_t speaker_count,
                            size_t* index,
                            float* distance);
int mradm_dsp_scene_rotation_create(const float* pose, size_t n, uint32_t contract, void** out);
void mradm_dsp_scene_rotation_destroy(void* h);
int mradm_dsp_scene_rotation_update(void* h, const float* pose, size_t n);
int mradm_dsp_scene_rotation_apply(const void* h, const float* src, size_t n, float* dst, size_t m, uint32_t apple);
int mradm_dsp_scene_rotate_pose(const float* src, size_t n, float* dst, size_t m, uint32_t apple);
int mradm_dsp_scene_pose(const float* src, size_t n, float* dst, size_t m, uint32_t quaternion);
#ifdef __cplusplus
}
#endif
// cppcheck-suppress-end unusedStructMember
