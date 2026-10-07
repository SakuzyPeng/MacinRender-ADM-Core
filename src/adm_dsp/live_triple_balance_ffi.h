#pragma once
#include <stddef.h>
#include <stdint.h>

#include "triple_balance_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// Private ABI; memory is valid/aligned, mutable handles exclusive, error buffers disjoint.
// Rust consumes these fields across the private ABI; isolated C++ analysis cannot see those reads.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmTbLiveElement {
    uint32_t kind;
    uint32_t reserved;
    float gains[24];
} MradmTbLiveElement;
typedef struct MradmTbLiveCommand {
    uint32_t element;
    uint32_t offset;
    uint32_t duration;
    uint32_t fields;
    MradmTbPosition position;
    float size;
    float level;
} MradmTbLiveCommand;
typedef struct MradmTbLivePlane {
    const float* samples;
    size_t length;
    uint32_t has_signal;
    uint32_t reserved;
} MradmTbLivePlane;
// cppcheck-suppress-end unusedStructMember
int mradm_dsp_live_tb_create(uint32_t layout,
                             uint32_t rate,
                             const MradmTbLiveElement* elements,
                             size_t element_count,
                             void** out,
                             char* error,
                             size_t capacity);
void mradm_dsp_live_tb_destroy(void* handle);
int mradm_dsp_live_tb_reset(void* handle, char* error, size_t capacity);
int mradm_dsp_live_tb_process(void* handle,
                              uint32_t frames,
                              const MradmTbLivePlane* planes,
                              size_t plane_count,
                              const MradmTbLiveCommand* initial,
                              size_t initial_count,
                              const MradmTbLiveCommand* events,
                              size_t event_count,
                              float* output,
                              size_t output_count,
                              char* error,
                              size_t capacity);
#ifdef __cplusplus
}
#endif
