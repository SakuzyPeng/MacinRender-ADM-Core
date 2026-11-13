#pragma once

// Private Rust boundary. Buffers must be valid, aligned, non-overlapping and
// live for the call. Mutable handles require exclusive access. Status values
// follow adm::ErrorCode; the optional message buffer is caller-owned UTF-8. Production C++ uses
// the gain banks and the meter; the entries only tests call are in tests/support/ffi/dsp_test_ffi.h.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Live gain banks and stereo device protection. Lengths count floats; empty buffers may be null.
int mradm_dsp_gain_create(
    size_t channels, uint32_t rate, uint32_t ramp_ms, void** output, char* message, size_t capacity);
void mradm_dsp_gain_destroy(void* handle);
int mradm_dsp_gain_reset(void* handle, char* message, size_t capacity);
int mradm_dsp_gain_set_targets(void* handle, const float* targets, size_t length, char* message, size_t capacity);
// mode 0 fills gain envelopes, mode 1 multiplies PCM in place.
int mradm_dsp_gain_process(void* handle, float* data, size_t length, uint32_t mode, char* message, size_t capacity);

// Private meter modes: 0=I, 1=TP, 2=I+TP, 3=M+S+I. Empty channel map
// preserves the legacy default mapping. Nonempty maps have one position per
// channel, using MeterChannel values from meter.h. Handles are worker-owned.
int32_t mradm_dsp_meter_create(uint32_t channels,
                               uint32_t sample_rate,
                               uint32_t mode,
                               const uint32_t* channel_map,
                               size_t map_length,
                               void** output,
                               char* message,
                               size_t capacity);
void mradm_dsp_meter_destroy(void* handle);
int32_t mradm_dsp_meter_add(void* handle, const float* input, size_t length, char* message, size_t capacity);
int32_t mradm_dsp_meter_reset(void* handle, char* message, size_t capacity);
// Queries: 0=I, 1=M, 2=S, 3=channel TP, 4=max TP. Silence/insufficient
// integrated history is successful -infinity; a silent peak is zero.
int32_t mradm_dsp_meter_query(
    const void* handle, uint32_t query, uint32_t channel, double* output, char* message, size_t capacity);

#ifdef __cplusplus
}
#endif
