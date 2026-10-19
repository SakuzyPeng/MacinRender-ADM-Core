#pragma once
#include <stddef.h>
#include <stdint.h>
// Private research ABI, not part of adm/c_api.h.
struct RoomGpuParams {
    uint32_t rate, fft, taps, bucket_stride, buckets, rows, images, history_parts;
    const float* materials;
    size_t material_floats;
    const float* propagation;
    size_t propagation_floats;
};
struct RoomGpuTerm {
    uint32_t row, fraction, shift;
    float alpha, weight;
};
struct RoomGpuJob {
    uint32_t material, fraction, shift, counts;
    float alpha, gain;
    RoomGpuTerm terms[12];
};
struct RoomGpuRange {
    uint32_t first, count;
};
struct RoomGpuUpdate {
    uint32_t slot, branch, range_first, range_count, parts, support;
};
struct RoomGpuTask {
    uint64_t start, end, block_start;
    uint32_t slot, branch, parts, begin, frames, first;
};
struct RoomGpuFrame {
    uint32_t active, reset, heads[2];
    const float* frequency;
    const RoomGpuJob* jobs;
    size_t job_count;
    const RoomGpuRange* ranges;
    size_t range_count;
    const RoomGpuUpdate* updates;
    size_t update_count;
    const RoomGpuTask* tasks;
    size_t task_count;
};
