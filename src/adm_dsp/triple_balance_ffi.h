#pragma once
#include <cstddef>
#include <cstdint>

// Private POD transport. No ownership or public ABI is exposed by these declarations.
// Buffers must be valid, aligned, disjoint and live for each call; mutable handles are exclusive.
// Lengths count elements. Error text uses a caller-owned UTF-8 buffer.
struct MradmTbPosition {
    float x, y, z;
};
struct MradmTbEvent {
    uint64_t start;
    MradmTbPosition position;
    float size;
};
struct MradmTbRow {
    std::size_t input, event_offset, event_count, bed_offset, size_index;
    uint32_t kind;
    float gain;
};
struct MradmTbQuery {
    MradmTbPosition position;
    float size, gain;
    uint32_t kind;
};
struct MradmTbNode {
    std::size_t channel;
    MradmTbPosition position;
    int32_t filter;
    float sign;
};
struct MradmTbObjectStatus {
    uint64_t control;
    std::size_t next_event, pending;
    MradmTbPosition position;
    float size;
    int32_t quantized[4];
    uint32_t flags;
};
extern "C" {
int mradm_dsp_tb_object_status(const void*, MradmTbObjectStatus*, char*, std::size_t);
int mradm_dsp_tb_gains(uint32_t, const MradmTbQuery*, std::size_t, float*, std::size_t, char*, std::size_t);
int mradm_dsp_tb_nodes(MradmTbNode*, std::size_t, char*, std::size_t);
int mradm_dsp_tb_quantize(MradmTbPosition, float, int32_t*, std::size_t, char*, std::size_t);
int mradm_dsp_tb_raw(const int32_t*, std::size_t, float*, std::size_t, char*, std::size_t);
int mradm_dsp_tb_size_mix(const float*, std::size_t, float, float*, std::size_t, uint32_t, char*, std::size_t);
int mradm_dsp_tb_plan_create(std::size_t,
                             uint32_t,
                             uint32_t,
                             uint64_t,
                             const MradmTbRow*,
                             std::size_t,
                             const MradmTbEvent*,
                             std::size_t,
                             const float*,
                             std::size_t,
                             void**,
                             char*,
                             std::size_t);
void mradm_dsp_tb_plan_destroy(void*);
int mradm_dsp_tb_plan_mix(const void*, void**, char*, std::size_t);
int mradm_dsp_tb_create(const void*, std::size_t, uint64_t, uint32_t, void**, char*, std::size_t);
void mradm_dsp_tb_destroy(void*);
int mradm_dsp_tb_reset(void*, char*, std::size_t);
int mradm_dsp_tb_scales(void*, const float*, std::size_t, uint32_t, char*, std::size_t);
int mradm_dsp_tb_prepare_points(void*, uint64_t, std::size_t, char*, std::size_t);
int mradm_dsp_tb_process(void*,
                         const float*,
                         std::size_t,
                         float*,
                         std::size_t,
                         const float*,
                         std::size_t,
                         uint64_t,
                         std::size_t,
                         uint32_t,
                         char*,
                         std::size_t);
int mradm_dsp_tb_point(void*,
                       std::size_t,
                       const float*,
                       std::size_t,
                       float*,
                       std::size_t,
                       const float*,
                       std::size_t,
                       uint64_t,
                       std::size_t,
                       uint32_t,
                       char*,
                       std::size_t);
int mradm_dsp_tb_snapshot_bytes(const void*, std::size_t*, char*, std::size_t);
int mradm_dsp_tb_snapshot_create(const void*, void**, char*, std::size_t);
void mradm_dsp_tb_snapshot_destroy(void*);
int mradm_dsp_tb_snapshot_capture(const void*, void*, char*, std::size_t);
int mradm_dsp_tb_snapshot_restore(void*, const void*, char*, std::size_t);
int mradm_dsp_tb_object_create(const MradmTbEvent*, std::size_t, uint32_t, uint32_t, void**, char*, std::size_t);
void mradm_dsp_tb_object_destroy(void*);
int mradm_dsp_tb_object_reset(void*, char*, std::size_t);
int mradm_dsp_tb_object_scale(void*, float, char*, std::size_t);
int mradm_dsp_tb_object_required(const void*, std::size_t, uint32_t, std::size_t*, char*, std::size_t);
int mradm_dsp_tb_object_process(
    void*, const float*, std::size_t, float*, std::size_t, uint32_t, std::size_t*, char*, std::size_t);
int mradm_dsp_tb_object_snapshot_create(const void*, void**, char*, std::size_t);
int mradm_dsp_tb_object_snapshot_restore(void*, const void*, char*, std::size_t);
void mradm_dsp_tb_object_snapshot_destroy(void*);
int mradm_dsp_tb_filter_create(void**, char*, std::size_t);
void mradm_dsp_tb_filter_destroy(void*);
int mradm_dsp_tb_filter_reset(void*, char*, std::size_t);
// Filter output length is a count of four-float frames.
int mradm_dsp_tb_filter_process(void*, const float*, std::size_t, float*, std::size_t, char*, std::size_t);
}
