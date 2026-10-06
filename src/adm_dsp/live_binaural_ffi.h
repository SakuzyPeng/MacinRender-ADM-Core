#pragma once
#include <cstddef>
#include <cstdint>
// POD fields cross the private Rust boundary.
// cppcheck-suppress-begin unusedStructMember
extern "C" {
struct MradmLiveBinauralState {
    uint64_t valid;
    uint32_t active;
    uint32_t channel_lock;
    uint32_t screen_reference;
    uint32_t head_locked;
    float gain;
    float position[3];
    float extent[3];
    float diffuse;
    float divergence;
    float divergence_range[2];
    uint32_t has_max_distance;
    float max_distance;
};
struct MradmLiveBinauralDescription {
    uint32_t role;
    uint32_t reserved;
    float fallback[2];
};
struct MradmLiveBinauralCommand {
    uint32_t element;
    uint32_t offset;
    uint32_t duration;
    uint32_t reserved;
    uint64_t changed;
    uint64_t cleared;
    MradmLiveBinauralState state;
    float direction[2];
    uint32_t has_direction;
    uint32_t diagnostic;
};
struct MradmLiveBinauralControl {
    MradmLiveBinauralState current;
    MradmLiveBinauralState target;
    float direction[2];
    float target_direction[2];
    uint32_t remaining[11];
    uint32_t has_direction;
    uint32_t initialized;
};
struct MradmLiveBinauralPlane {
    const float* samples;
    size_t length;
    uint32_t has_signal;
    uint32_t reserved;
};
struct MradmLiveBinauralDiagnostic {
    uint32_t element;
    uint32_t kind;
};
struct MradmLiveBinauralReport {
    uint32_t count;
    uint32_t mask;
    MradmLiveBinauralDiagnostic records[4];
};
int mradm_dsp_live_binaural_create(
    const void*, const MradmLiveBinauralDescription*, size_t, uint32_t, uint32_t, uint32_t, void**, char*, size_t);
void mradm_dsp_live_binaural_destroy(void*);
int mradm_dsp_live_binaural_reset(void*);
int mradm_dsp_live_binaural_process(void*,
                                    uint32_t,
                                    const MradmLiveBinauralPlane*,
                                    size_t,
                                    const MradmLiveBinauralCommand*,
                                    size_t,
                                    const MradmLiveBinauralCommand*,
                                    size_t,
                                    const float*,
                                    size_t,
                                    uint32_t,
                                    float*,
                                    size_t,
                                    MradmLiveBinauralReport*,
                                    char*,
                                    size_t);
int mradm_dsp_live_binaural_control(const void*, uint32_t, MradmLiveBinauralControl*);
}

// cppcheck-suppress-end unusedStructMember
