#pragma once
#include <stddef.h>
#include <stdint.h>

// Private transport only. Buffers are valid, aligned and disjoint; mutable handles are exclusive.
// Lengths count elements; zero-length buffers may be null. The caller owns UTF-8 error storage.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MradmHoaSource {
    float position[3]; // Cartesian XYZ, or polar azimuth/elevation/distance
    uint32_t cartesian;
    float width, height, depth, gain, diffuse;
} MradmHoaSource;
typedef struct MradmHoaBlock {
    uint64_t start, end, interpolation;
    size_t source_offset, source_count;
    float object_gain;
    uint32_t kind;  // 0 Objects, 1 directional DirectSpeakers, 2 LFE
    uint32_t flags; // 1 jump, 2 explicit interpolation
} MradmHoaBlock;
typedef struct MradmHoaRow {
    size_t input, block_offset, block_count;
} MradmHoaRow;
typedef struct MradmHoaTrace {
    uint32_t flags;
    float polar[8], direction[3], normalized[3], coefficients[16];
} MradmHoaTrace;
int mradm_dsp_hoa_plan_create(size_t inputs,
                              const MradmHoaRow* rows,
                              size_t n,
                              const MradmHoaBlock* blocks,
                              size_t bn,
                              const size_t* order,
                              size_t on,
                              const MradmHoaSource* sources,
                              size_t sn,
                              void** out,
                              MradmHoaTrace* trace,
                              char* message,
                              size_t capacity);
void mradm_dsp_hoa_plan_destroy(void* p);
int mradm_dsp_hoa_plan_has_lfe(const void* plan, uint32_t* out, char* message, size_t capacity);
int mradm_dsp_hoa_coefficients(
    const void* plan, size_t row, size_t block, float* out, size_t len, char* message, size_t capacity);
int mradm_dsp_hoa_encoder_create(const void* plan,
                                 size_t max_frames,
                                 uint64_t interpolation,
                                 uint32_t smoothing,
                                 void** out,
                                 char* message,
                                 size_t capacity);
void mradm_dsp_hoa_encoder_destroy(void* p);
int mradm_dsp_hoa_encoder_reset(void* p, uint64_t start, char* message, size_t capacity);
int mradm_dsp_hoa_encode(void* p,
                         const float* src,
                         size_t n,
                         float* out,
                         size_t len,
                         uint64_t start,
                         size_t frames,
                         char* message,
                         size_t capacity);
int mradm_dsp_hoa_meter_create(
    const void* plan, size_t max_frames, uint64_t interpolation, void** out, char* message, size_t capacity);
void mradm_dsp_hoa_meter_destroy(void* p);
int mradm_dsp_hoa_meter_reset(void* p, uint64_t start, char* message, size_t capacity);
int mradm_dsp_hoa_meter_process(void* p,
                                const float* src,
                                size_t n,
                                const float* hoa,
                                size_t hn,
                                float* decoded,
                                size_t dn,
                                float* lfe,
                                size_t ln,
                                uint64_t start,
                                size_t frames,
                                char* message,
                                size_t capacity);

#ifdef __cplusplus
}
#endif
