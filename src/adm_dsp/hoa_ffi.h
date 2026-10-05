#pragma once
#include <cstddef>
#include <cstdint>

// Private transport only. Buffers are valid, aligned and disjoint; mutable handles are exclusive.
// Lengths count elements; zero-length buffers may be null. The caller owns UTF-8 error storage.
struct MradmHoaSource {
    float position[3]; // Cartesian XYZ, or polar azimuth/elevation/distance
    uint32_t cartesian;
    float width, height, depth, gain, diffuse;
};
struct MradmHoaBlock {
    uint64_t start, end, interpolation;
    std::size_t source_offset, source_count;
    float object_gain;
    uint32_t kind;  // 0 Objects, 1 directional DirectSpeakers, 2 LFE
    uint32_t flags; // 1 jump, 2 explicit interpolation
};
struct MradmHoaRow {
    std::size_t input, block_offset, block_count;
};
struct MradmHoaTrace {
    uint32_t flags;
    float polar[8], direction[3], normalized[3], coefficients[16];
};
extern "C" {
int mradm_dsp_hoa_plan_create(std::size_t,
                              const MradmHoaRow*,
                              std::size_t,
                              const MradmHoaBlock*,
                              std::size_t,
                              const std::size_t*,
                              std::size_t,
                              const MradmHoaSource*,
                              std::size_t,
                              void**,
                              MradmHoaTrace*,
                              char*,
                              std::size_t);
void mradm_dsp_hoa_plan_destroy(void*);
int mradm_dsp_hoa_plan_has_lfe(const void*, uint32_t*, char*, std::size_t);
int mradm_dsp_hoa_coefficients(const void*, std::size_t, std::size_t, float*, std::size_t, char*, std::size_t);
int mradm_dsp_hoa_encoder_create(const void*, std::size_t, uint64_t, uint32_t, void**, char*, std::size_t);
void mradm_dsp_hoa_encoder_destroy(void*);
int mradm_dsp_hoa_encoder_reset(void*, uint64_t, char*, std::size_t);
int mradm_dsp_hoa_encode(
    void*, const float*, std::size_t, float*, std::size_t, uint64_t, std::size_t, char*, std::size_t);
int mradm_dsp_hoa_meter_create(const void*, std::size_t, uint64_t, void**, char*, std::size_t);
void mradm_dsp_hoa_meter_destroy(void*);
int mradm_dsp_hoa_meter_reset(void*, uint64_t, char*, std::size_t);
int mradm_dsp_hoa_meter_process(void*,
                                const float*,
                                std::size_t,
                                const float*,
                                std::size_t,
                                float*,
                                std::size_t,
                                float*,
                                std::size_t,
                                uint64_t,
                                std::size_t,
                                char*,
                                std::size_t);
}
