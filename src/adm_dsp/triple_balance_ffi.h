#pragma once
#include <stddef.h>
#include <stdint.h>

// Private POD transport. No ownership or public ABI is exposed by these declarations.
// Buffers must be valid, aligned, disjoint and live for each call; mutable handles are exclusive.
// Lengths count elements. Error text uses a caller-owned UTF-8 buffer.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MradmTbPosition {
    float x, y, z;
} MradmTbPosition;
typedef struct MradmTbEvent {
    uint64_t start;
    MradmTbPosition position;
    float size;
} MradmTbEvent;
typedef struct MradmTbRow {
    size_t input, event_offset, event_count, bed_offset, size_index;
    uint32_t kind;
    float gain;
} MradmTbRow;
typedef struct MradmTbQuery {
    MradmTbPosition position;
    float size, gain;
    uint32_t kind;
} MradmTbQuery;
typedef struct MradmTbNode {
    size_t channel;
    MradmTbPosition position;
    int32_t filter;
    float sign;
} MradmTbNode;
typedef struct MradmTbObjectStatus {
    uint64_t control;
    size_t next_event, pending;
    MradmTbPosition position;
    float size;
    int32_t quantized[4];
    uint32_t flags;
} MradmTbObjectStatus;
int mradm_dsp_tb_d_plan_create(size_t inputs,
                               uint32_t layout,
                               uint64_t total,
                               const MradmTbRow* rows,
                               size_t n,
                               const MradmTbEvent* events,
                               size_t en,
                               const float* bed,
                               size_t bn,
                               void** out,
                               char* message,
                               size_t capacity);
void mradm_dsp_tb_d_plan_destroy(void* p);
int mradm_dsp_tb_d_create(const void* plan, void** out, char* message, size_t capacity);
void mradm_dsp_tb_d_destroy(void* p);
int mradm_dsp_tb_d_process(void* state,
                           const float* src,
                           size_t src_len,
                           float* out,
                           size_t out_len,
                           uint64_t start,
                           size_t frames,
                           char* message,
                           size_t capacity);
int mradm_dsp_tb_object_status(const void* p, MradmTbObjectStatus* out, char* message, size_t capacity);
int mradm_dsp_tb_gains(
    uint32_t layout, const MradmTbQuery* queries, size_t n, float* out, size_t len, char* message, size_t capacity);
int mradm_dsp_tb_nodes(MradmTbNode* out, size_t len, char* message, size_t capacity);
int mradm_dsp_tb_quantize(MradmTbPosition p, float size, int32_t* out, size_t len, char* message, size_t capacity);
int mradm_dsp_tb_raw(const int32_t* q, size_t n, float* out, size_t len, char* message, size_t capacity);
int mradm_dsp_tb_size_mix(
    const float* raw, size_t n, float size, float* out, size_t len, uint32_t extended, char* message, size_t capacity);
int mradm_dsp_tb_plan_create(size_t inputs,
                             uint32_t layout,
                             uint32_t rate,
                             uint64_t total,
                             const MradmTbRow* rows,
                             size_t n,
                             const MradmTbEvent* events,
                             size_t en,
                             const float* bed,
                             size_t bn,
                             void** out,
                             char* message,
                             size_t capacity);
void mradm_dsp_tb_plan_destroy(void* p);
int mradm_dsp_tb_plan_mix(const void* plan, void** out, char* message, size_t capacity);
int mradm_dsp_tb_create(
    const void* plan, size_t max, uint64_t interpolation, uint32_t live, void** out, char* message, size_t capacity);
void mradm_dsp_tb_destroy(void* p);
int mradm_dsp_tb_reset(void* p, char* message, size_t capacity);
int mradm_dsp_tb_scales(void* p, const float* scales, size_t len, uint32_t mode, char* message, size_t capacity);
int mradm_dsp_tb_prepare_points(void* p, uint64_t start, size_t frames, char* message, size_t capacity);
int mradm_dsp_tb_process(void* p,
                         const float* src,
                         size_t src_len,
                         float* out,
                         size_t out_len,
                         const float* live,
                         size_t live_len,
                         uint64_t start,
                         size_t frames,
                         uint32_t final_block,
                         char* message,
                         size_t capacity);
int mradm_dsp_tb_point(void* p,
                       size_t track,
                       const float* src,
                       size_t src_len,
                       float* out,
                       size_t out_len,
                       const float* live,
                       size_t live_len,
                       uint64_t start,
                       size_t frames,
                       uint32_t user_gain,
                       char* message,
                       size_t capacity);
int mradm_dsp_tb_snapshot_bytes(const void* p, size_t* out, char* message, size_t capacity);
int mradm_dsp_tb_snapshot_create(const void* p, void** out, char* message, size_t capacity);
void mradm_dsp_tb_snapshot_destroy(void* p);
int mradm_dsp_tb_snapshot_capture(const void* p, void* out, char* message, size_t capacity);
int mradm_dsp_tb_snapshot_restore(void* p, const void* state, char* message, size_t capacity);
int mradm_dsp_tb_object_create(
    const MradmTbEvent* events, size_t len, uint32_t layout, uint32_t rate, void** out, char* message, size_t capacity);
void mradm_dsp_tb_object_destroy(void* p);
int mradm_dsp_tb_object_reset(void* p, char* message, size_t capacity);
int mradm_dsp_tb_object_scale(void* p, float scale, char* message, size_t capacity);
int mradm_dsp_tb_object_required(
    const void* p, size_t input, uint32_t finish, size_t* out, char* message, size_t capacity);
int mradm_dsp_tb_object_process(void* p,
                                const float* src,
                                size_t n,
                                float* out,
                                size_t len,
                                uint32_t finish,
                                size_t* produced,
                                char* message,
                                size_t capacity);
int mradm_dsp_tb_object_snapshot_create(const void* p, void** out, char* message, size_t capacity);
int mradm_dsp_tb_object_snapshot_restore(void* p, const void* state, char* message, size_t capacity);
void mradm_dsp_tb_object_snapshot_destroy(void* p);
int mradm_dsp_tb_filter_create(void** out, char* message, size_t capacity);
void mradm_dsp_tb_filter_destroy(void* p);
int mradm_dsp_tb_filter_reset(void* p, char* message, size_t capacity);
// Filter output length is a count of four-float frames.
int mradm_dsp_tb_filter_process(
    void* p, const float* src, size_t n, float* out, size_t len, char* message, size_t capacity);

#ifdef __cplusplus
}
#endif
