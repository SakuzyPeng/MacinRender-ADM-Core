#pragma once
#include <stddef.h>
#include <stdint.h>
// POD fields cross the private Rust boundary.
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmLiveBinauralState {
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
} MradmLiveBinauralState;
typedef struct MradmLiveBinauralDescription {
    uint32_t role;
    uint32_t reserved;
    float fallback[2];
} MradmLiveBinauralDescription;
typedef struct MradmLiveBinauralCommand {
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
} MradmLiveBinauralCommand;
typedef struct MradmLiveBinauralControl {
    MradmLiveBinauralState current;
    MradmLiveBinauralState target;
    float direction[2];
    float target_direction[2];
    uint32_t remaining[11];
    uint32_t has_direction;
    uint32_t initialized;
} MradmLiveBinauralControl;
typedef struct MradmLiveBinauralPlane {
    const float* samples;
    size_t length;
    uint32_t has_signal;
    uint32_t reserved;
} MradmLiveBinauralPlane;
typedef struct MradmLiveBinauralDiagnostic {
    uint32_t element;
    uint32_t kind;
} MradmLiveBinauralDiagnostic;
typedef struct MradmLiveBinauralReport {
    uint32_t count;
    uint32_t mask;
    MradmLiveBinauralDiagnostic records[4];
} MradmLiveBinauralReport;
int mradm_dsp_live_binaural_create(const void* filters,
                                   const MradmLiveBinauralDescription* descriptions,
                                   size_t count,
                                   uint32_t rate,
                                   uint32_t spread,
                                   uint32_t contract,
                                   void** out,
                                   char* message,
                                   size_t capacity);
void mradm_dsp_live_binaural_destroy(void* h);
int mradm_dsp_live_binaural_reset(void* h);
int mradm_dsp_live_binaural_process(void* h,
                                    uint32_t frames,
                                    const MradmLiveBinauralPlane* planes,
                                    size_t plane_count,
                                    const MradmLiveBinauralCommand* initial,
                                    size_t initial_count,
                                    const MradmLiveBinauralCommand* events,
                                    size_t event_count,
                                    const float* pose,
                                    size_t pose_len,
                                    uint32_t warned,
                                    float* pcm,
                                    size_t pcm_len,
                                    MradmLiveBinauralReport* report,
                                    char* message,
                                    size_t capacity);
int mradm_dsp_live_binaural_control(const void* h, uint32_t index, MradmLiveBinauralControl* out);

#ifdef __cplusplus
}
#endif
// cppcheck-suppress-end unusedStructMember
