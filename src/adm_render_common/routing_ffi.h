// Rust mradm-render records the Apple island uses: the routing matrix, override and LFE routing
// records, the live-override and LFE gain rules, and the backend preparation handle. Inputs are
// borrowed for the call. The entries only tests and tools call are in
// tests/support/ffi/routing_test_ffi.h and tests/support/render_test_ffi.h. Status values follow
// adm::ErrorCode.
#pragma once
#include "../adm_metadata/adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif

// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRenderMatrixTarget {
    MradmAdmText label;
    float gain;
} MradmRenderMatrixTarget;

typedef struct MradmRenderMatrixRoute {
    MradmAdmText source_label;
    uint32_t mute;
    const MradmRenderMatrixTarget* targets;
    size_t targets_len;
} MradmRenderMatrixRoute;

typedef struct MradmRenderOverride {
    MradmAdmText object_id;
    MradmAdmText speaker_label;
    uint32_t mute;
    float gain_db;
    uint32_t has_head_locked;
    uint32_t head_locked;
} MradmRenderOverride;

// level: 1 info, 2 warning (adm::LogLevel); message lives as long as its owner.
typedef struct MradmRenderLogLine {
    uint32_t level;
    MradmAdmText message;
} MradmRenderLogLine;

typedef struct MradmRenderLfeRouting {
    uint32_t applies_to_22_2;
    uint32_t split_power;
    uint32_t has_lfe1;
    uint32_t has_lfe2;
    MradmRenderLogLine logs[2];
    size_t logs_len;
} MradmRenderLfeRouting;

// A live edit (`LiveObjectOverride`): the common override and the topology scales.
typedef struct MradmRenderLiveOverride {
    MradmRenderOverride base;
    float extent_scale;
    float extent_width_scale;
    float extent_height_scale;
    float extent_depth_scale;
    float diffuse_scale;
    float divergence_scale;
} MradmRenderLiveOverride;
// AudioDeviceProgress.
typedef struct MradmEngineSceneDeviceProgress {
    uint64_t presented_frames;
    uint32_t media_clock;
    uint32_t failed;
    uint32_t recovering;
    uint64_t underruns;
} MradmEngineSceneDeviceProgress;
// cppcheck-suppress-end unusedStructMember

// Bit 1: *gain written; bit 2: *head_locked written; otherwise the channel inherits.
uint32_t mradm_render_live_override(const MradmRenderOverride* values,
                                    size_t values_len,
                                    MradmAdmText object_id,
                                    MradmAdmText key,
                                    float* gain,
                                    uint32_t* head_locked);
float mradm_render_lfe_gain(const MradmRenderLfeRouting* routing, uint32_t input_target, uint32_t output_target);

#ifdef __cplusplus
}
#endif
