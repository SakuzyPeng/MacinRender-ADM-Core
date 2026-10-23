#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Private ABI; memory is valid/aligned, mutable handles exclusive, error buffers disjoint.
// Rust consumes these fields across the private ABI; isolated C++ analysis cannot see those reads.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmLiveVbapCommand {
    uint32_t element;
    uint32_t offset;
    uint32_t duration;
    uint32_t fields; // 1 = panning, 2 = level; initial commands require both and zero offset/duration.
    uint64_t coefficient_offset;
    float level;
    uint32_t reserved;
} MradmLiveVbapCommand;
typedef struct MradmLiveVbapPlane {
    const float* samples;
    size_t length;
    uint32_t has_signal;
    uint32_t reserved;
} MradmLiveVbapPlane;
typedef struct MradmLiveVbapStatus {
    float current_level;
    float target_level;
    float level_step;
    uint32_t level_remaining;
    uint32_t pan_remaining;
} MradmLiveVbapStatus;
// cppcheck-suppress-end unusedStructMember
int mradm_dsp_live_vbap_create(uint32_t elements, uint32_t channels, void** out, char* error, size_t capacity);
void mradm_dsp_live_vbap_destroy(void* handle);
int mradm_dsp_live_vbap_reset(void* handle, char* error, size_t capacity);
int mradm_dsp_live_vbap_process(void* handle,
                                uint32_t frames,
                                const MradmLiveVbapPlane* planes,
                                size_t plane_count,
                                const MradmLiveVbapCommand* initial,
                                size_t initial_count,
                                const MradmLiveVbapCommand* events,
                                size_t event_count,
                                const float* coefficients,
                                size_t coefficient_count,
                                float* output,
                                size_t output_count,
                                char* error,
                                size_t capacity);
int mradm_dsp_live_vbap_snapshot(const void* handle,
                                 uint32_t element,
                                 float* gains,
                                 size_t gain_count,
                                 MradmLiveVbapStatus* status,
                                 char* error,
                                 size_t capacity);
#ifdef __cplusplus
}
#endif
