#pragma once
#include <stddef.h>
#include <stdint.h>
// POD fields cross the private Rust boundary.
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmSceneTransitionStatus {
    uint64_t backend_position;
    uint64_t generation_position;
    uint64_t generation_remaining;
} MradmSceneTransitionStatus;
int mradm_dsp_scene_transition_create(size_t channels, uint32_t rate, uint64_t frames, void** out);
void mradm_dsp_scene_transition_destroy(void* h);
int mradm_dsp_scene_transition_control(void* h, uint32_t command);
int mradm_dsp_scene_transition_status(const void* h, MradmSceneTransitionStatus* out);
int mradm_dsp_scene_transition_mix(
    void* h, float* old, size_t old_len, const float* incoming, size_t incoming_len, size_t frames, uint32_t* done);
int mradm_dsp_scene_transition_output(void* h, float* pcm, size_t length, size_t frames, uint32_t silence);
int mradm_dsp_scene_transition_snapshot(
    const void* h, float* last, size_t last_len, float* anchor, size_t anchor_len, MradmSceneTransitionStatus* status);
#ifdef __cplusplus
}
#endif
// cppcheck-suppress-end unusedStructMember
