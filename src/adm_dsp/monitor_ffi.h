#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Private, thread-exclusive handles. Buffers are aligned, live, and disjoint;
// lengths count float elements. Error storage must not alias any other argument.
int mradm_dsp_monitor_crossfade_create(size_t channels, uint64_t frames, void** out, char* error, size_t capacity);
void mradm_dsp_monitor_crossfade_destroy(void* handle);
int mradm_dsp_monitor_crossfade_reset(void* handle, char* error, size_t capacity);
int mradm_dsp_monitor_crossfade_process(void* handle,
                                        float* old_pcm,
                                        size_t old_len,
                                        const float* incoming,
                                        size_t incoming_len,
                                        size_t frames,
                                        uint32_t* complete,
                                        char* error,
                                        size_t capacity);
int mradm_dsp_monitor_output_create(
    size_t channels, uint32_t rate, uint32_t realtime, void** out, char* error, size_t capacity);
void mradm_dsp_monitor_output_destroy(void* handle);
int mradm_dsp_monitor_output_reset(void* handle, char* error, size_t capacity);
int mradm_dsp_monitor_output_process(void* handle,
                                     float* pcm,
                                     size_t pcm_len,
                                     size_t frames,
                                     size_t produced,
                                     uint32_t active,
                                     uint64_t generation,
                                     float* peak,
                                     size_t peak_len,
                                     float* rms,
                                     size_t rms_len,
                                     char* error,
                                     size_t capacity);
#ifdef __cplusplus
}
#endif
