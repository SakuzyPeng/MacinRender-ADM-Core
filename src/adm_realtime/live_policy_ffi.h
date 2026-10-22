// Scene stream semantic policy in Rust mradm-scene `live_policy` (PCM consistency 4.4). Handles are
// immutable once made and destroyed by their own *_destroy; a resolution's `unmatched` array lives
// as long as the resolution. Status values follow adm::ErrorCode.
#pragma once
#include "adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmLivePolicy MradmLivePolicy;
typedef struct MradmLiveGeneration MradmLiveGeneration;
typedef struct MradmLiveResolution MradmLiveResolution;

typedef struct MradmLivePolicyEntity {
    MradmAdmText id;
    MradmAdmText name;
} MradmLivePolicyEntity;

typedef struct MradmLivePolicyIdentity {
    uint32_t present;
    MradmAdmText object_id;
    MradmAdmText object_name;
    MradmAdmText track_uid;
    uint32_t has_importance;
    int32_t importance;
    uint32_t has_dialogue_id;
    int32_t dialogue_id;
    const MradmLivePolicyEntity* contents;
    size_t contents_len;
    const MradmLivePolicyEntity* programmes;
    size_t programmes_len;
} MradmLivePolicyIdentity;

// role: 0 object, 1 DirectSpeakers, 2 LFE.
typedef struct MradmLivePolicyElement {
    uint32_t role;
    MradmAdmText speaker_label;
    uint32_t has_position;
    float position[3];
    MradmLivePolicyIdentity identity;
} MradmLivePolicyElement;

// has_position: in, the producer position is valid; out (DirectSpeakers), the effective one is.
typedef struct MradmLivePolicyState {
    uint32_t has_position;
    float linear_gain;
    float position[3];
    float width;
    float height;
    float depth;
    float diffuse;
    float divergence;
    uint32_t channel_lock;
    uint32_t screen_reference;
    uint32_t head_locked;
    float divergence_azimuth_range;
    float divergence_position_range;
    uint32_t has_max_distance;
    float max_distance;
} MradmLivePolicyState;

typedef struct MradmLivePolicyUpdate {
    uint32_t jump_position;
    uint32_t ramp_duration_samples;
} MradmLivePolicyUpdate;
// cppcheck-suppress-end unusedStructMember

// Policy errors set context 3 (the caller's source label).
int32_t mradm_live_policy_parse(const uint8_t* json,
                                size_t json_len,
                                MradmLivePolicy** output,
                                uint32_t* context,
                                uint8_t* message,
                                size_t capacity);
void mradm_live_policy_destroy(MradmLivePolicy* handle);
int32_t mradm_live_policy_generation(const MradmLivePolicyElement* elements,
                                     size_t elements_len,
                                     MradmLiveGeneration** output,
                                     uint8_t* message,
                                     size_t capacity);
void mradm_live_policy_generation_destroy(MradmLiveGeneration* handle);
int32_t mradm_live_policy_resolve(const MradmLivePolicy* policy,
                                  const MradmLiveGeneration* generation,
                                  MradmLiveResolution** output,
                                  const size_t** unmatched,
                                  size_t* unmatched_len,
                                  uint8_t* message,
                                  size_t capacity);
void mradm_live_policy_resolution_destroy(MradmLiveResolution* handle);
// update may be null; does not allocate.
int32_t mradm_live_policy_apply(const MradmLiveResolution* resolution,
                                size_t index,
                                uint32_t sample_rate,
                                MradmLivePolicyState* state,
                                MradmLivePolicyUpdate* update);

#ifdef __cplusplus
}
#endif
