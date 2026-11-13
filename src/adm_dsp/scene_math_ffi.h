// Scene spatial math of Rust mradm-dsp used by the Apple island (scene_math.h). The entries only
// tests call are in tests/support/ffi/scene_math_test_ffi.h.
#pragma once
#include <stddef.h>
#include <stdint.h>
// POD fields cross the private Rust boundary.
#ifdef __cplusplus
extern "C" {
#endif

int mradm_dsp_scene_math(uint32_t op, const float* src, size_t n, float* dst, size_t m);
int mradm_dsp_scene_rotation_create(const float* pose, size_t n, uint32_t contract, void** out);
void mradm_dsp_scene_rotation_destroy(void* h);
int mradm_dsp_scene_rotation_update(void* h, const float* pose, size_t n);
int mradm_dsp_scene_rotation_apply(const void* h, const float* src, size_t n, float* dst, size_t m, uint32_t apple);
#ifdef __cplusplus
}
#endif
