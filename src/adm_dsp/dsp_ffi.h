#pragma once

// Private Rust boundary. Buffers must be valid, aligned, non-overlapping and
// live for the call. Mutable handles require exclusive access. Status values
// follow adm::ErrorCode; the optional message buffer is caller-owned UTF-8.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

int32_t mradm_dsp_fft_create(size_t length, void** output, char* message, size_t capacity);
void mradm_dsp_fft_destroy(void* handle);
int32_t mradm_dsp_fft_forward(void* handle,
                              const float* input,
                              size_t input_length,
                              float* output,
                              size_t output_length,
                              char* message,
                              size_t capacity);
int32_t mradm_dsp_fft_inverse(void* handle,
                              const float* input,
                              size_t input_length,
                              float* output,
                              size_t output_length,
                              char* message,
                              size_t capacity);

int32_t mradm_dsp_panner_create(
    const float* directions, size_t length, int32_t is_3d, void** output, char* message, size_t capacity);
void mradm_dsp_panner_destroy(void* handle);
int32_t mradm_dsp_panner_gains(const void* handle,
                               float azimuth,
                               float elevation,
                               float spread,
                               float* output,
                               size_t length,
                               char* message,
                               size_t capacity);
int32_t mradm_dsp_hrtf_grid(const float* directions,
                            size_t length,
                            float* weights,
                            int32_t* indices,
                            size_t output_length,
                            char* message,
                            size_t capacity);
int32_t mradm_dsp_hrir_transform(const float* input,
                                 size_t input_length,
                                 size_t directions,
                                 size_t taps,
                                 size_t fft_length,
                                 float* output,
                                 size_t output_length,
                                 char* message,
                                 size_t capacity);
// Filled by Rust and consumed by other translation units; keep a C-compatible POD.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspDatasetInfo {
    uint32_t sample_rate;
    size_t num_dirs;
    size_t ir_len;
} MradmDspDatasetInfo;
// cppcheck-suppress-end unusedStructMember
int32_t mradm_dsp_dataset_create(
    const uint8_t* bytes, size_t length, int32_t builtin, void** output, char* message, size_t capacity);
void mradm_dsp_dataset_destroy(void* handle);
int32_t mradm_dsp_dataset_info(const void* handle, MradmDspDatasetInfo* info, char* message, size_t capacity);
int32_t mradm_dsp_dataset_copy(const void* handle,
                               float* directions,
                               size_t dir_length,
                               float* impulses,
                               size_t ir_length,
                               char* name,
                               size_t name_capacity,
                               char* message,
                               size_t capacity);
int32_t mradm_dsp_hoa_matrix(float* output, size_t length, char* message, size_t capacity);

int32_t mradm_dsp_spreader_create(const float* ir,
                                  size_t ir_length,
                                  const float* directions,
                                  size_t dir_length,
                                  size_t taps,
                                  uint32_t sample_rate,
                                  const uint64_t* seeds,
                                  size_t sources,
                                  void** output,
                                  char* message,
                                  size_t capacity);
void mradm_dsp_spreader_destroy(void* handle);
int32_t mradm_dsp_spreader_set_source(
    void* handle, size_t index, float azimuth, float elevation, float spread, char* message, size_t capacity);
int32_t mradm_dsp_spreader_process(void* handle,
                                   const float* const* inputs,
                                   size_t sources,
                                   float* left,
                                   float* right,
                                   size_t frames,
                                   char* message,
                                   size_t capacity);
uint32_t mradm_dsp_spreader_delay(void);

#ifdef __cplusplus
}
#endif
