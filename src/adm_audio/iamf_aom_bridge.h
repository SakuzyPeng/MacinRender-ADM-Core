#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MR_IAMF_AOM_BRIDGE_ABI_VERSION 1

typedef enum MrIamfAomProfile {
    MR_IAMF_AOM_PROFILE_AUTO = 0,
    MR_IAMF_AOM_PROFILE_SIMPLE = 1,
    MR_IAMF_AOM_PROFILE_BASE = 2,
    MR_IAMF_AOM_PROFILE_BASE_ENHANCED = 3,
    MR_IAMF_AOM_PROFILE_BASE_ADVANCED = 4,
    MR_IAMF_AOM_PROFILE_ADVANCED1 = 5,
    MR_IAMF_AOM_PROFILE_ADVANCED2 = 6,
} MrIamfAomProfile;

typedef struct MrIamfAomEncodeOptions {
    uint32_t abi_version;
    const char* input_wav_path;
    const char* output_iamf_path;
    const char* layout_id;
    uint32_t opus_bitrate_per_ch_kbps;
    MrIamfAomProfile profile;
    int has_loudness_lufs;
    double loudness_lufs;
    int has_peak_dbtp;
    double peak_dbtp;
} MrIamfAomEncodeOptions;

uint32_t mr_iamf_aom_bridge_abi_version(void);

int mr_iamf_aom_encode_wav_to_iamf(const MrIamfAomEncodeOptions* options, char* error_buffer, size_t error_buffer_size);

#ifdef __cplusplus
}
#endif
