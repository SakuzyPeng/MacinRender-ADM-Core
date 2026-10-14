#pragma once
#include <stddef.h>
#include <stdint.h>
// POD fields cross the private Rust boundary; see mradm-dsp `semantic_policy`.
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmPolicyValue {
    uint32_t present;
    float value;
} MradmPolicyValue;
typedef struct MradmPolicyGain {
    MradmPolicyValue scale;
    MradmPolicyValue gain_db;
} MradmPolicyGain;
typedef struct MradmPolicyPosition {
    uint32_t present;
    MradmPolicyValue azimuth;
    MradmPolicyValue elevation;
    MradmPolicyValue distance;
    uint32_t has_offset;
    MradmPolicyValue offset_azimuth;
    MradmPolicyValue offset_elevation;
    MradmPolicyValue offset_distance;
    MradmPolicyValue lock_azimuth;
    MradmPolicyValue lock_elevation;
} MradmPolicyPosition;
// Section modes: 0 absent, 1 disabled, 2 scaled.
typedef struct MradmPolicyObject {
    uint32_t diffuse;
    MradmPolicyValue diffuse_scale;
    MradmPolicyValue diffuse_max;
    uint32_t extent;
    MradmPolicyValue extent_scale;
    MradmPolicyValue width_scale;
    MradmPolicyValue height_scale;
    MradmPolicyValue depth_scale;
    MradmPolicyValue extent_max;
    uint32_t divergence;
    MradmPolicyValue divergence_scale;
    MradmPolicyValue divergence_range_scale;
    MradmPolicyValue divergence_max_range;
    MradmPolicyPosition position;
    uint32_t interpolation_max_ms;
} MradmPolicyObject;
typedef struct MradmPolicyBlock {
    uint32_t cartesian;
    float position[3];
    float diffuse;
    float width;
    float height;
    float depth;
    float divergence;
    float divergence_azimuth_range;
    float divergence_position_range;
    uint32_t has_interpolation;
    uint64_t interpolation_samples;
} MradmPolicyBlock;
typedef struct MradmPolicyDirect {
    MradmPolicyGain gain;
    uint32_t mute;
    MradmPolicyPosition position;
} MradmPolicyDirect;
typedef struct MradmPolicyDirectBlock {
    float gain;
    float user_gain_multiplier;
    uint32_t has_position;
    float polar[3];
    uint32_t user_position_override;
} MradmPolicyDirectBlock;
int mradm_dsp_policy_gain(const MradmPolicyGain* rule, float* values, size_t n);
int mradm_dsp_policy_object(const MradmPolicyObject* rule, uint32_t sample_rate, MradmPolicyBlock* block);
int mradm_dsp_policy_direct(const MradmPolicyDirect* rule, MradmPolicyDirectBlock* block);
int mradm_dsp_policy_scene_state(const float* src, size_t n, uint32_t polar, float* dst, size_t m);
#ifdef __cplusplus
}
#endif
// cppcheck-suppress-end unusedStructMember
