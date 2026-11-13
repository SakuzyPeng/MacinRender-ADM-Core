/* The context and output strings of the Rust C ABI (mradm-capi), for the exports still in adm_c_api.cpp. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "adm/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies len bytes (0 clears it). NULL context: OK. Allocation failure: INTERNAL, previous message retained. */
adm_error_code_t mradm_capi_context_set_error(adm_context_t* context, const uint8_t* message, size_t len);

/* A copy of len bytes of text and a terminating NUL, released with adm_free_string; NULL when out of memory. */
char* mradm_capi_string(const uint8_t* text, size_t len);

/* A new owner of the room's package (destroyed with mradm_room_profile_destroy); NULL for a NULL room or OOM. */
void* mradm_capi_room_profile(const adm_room_t* room);

/* Bytes the options own. */
typedef struct MradmCapiText {
    const uint8_t* data;
    size_t len;
} MradmCapiText;

/* A screen geometry (ScreenGeometry). */
typedef struct MradmCapiScreen {
    uint32_t cartesian;
    double aspect_ratio;
    double centre[3];
    double width;
} MradmCapiScreen;

/* The render options as the C++ RenderOptions the Monitor exports still build: enums as their values,
   paths as UTF-8, room_profile a new owner of the room's package (destroyed with
   mradm_room_profile_destroy) or NULL. */
typedef struct MradmCapiOptions {
    uint32_t renderer;
    MradmCapiText output_layout;
    uint32_t has_input_layout;
    MradmCapiText input_layout;
    const MradmCapiText* input_channel_labels;
    size_t input_channel_labels_len;
    uint32_t monitor_system_spatial;
    uint32_t measure_loudness;
    float loudness_target_lufs;
    uint32_t peak_limit;
    float peak_limit_dbtp;
    uint32_t peak_normalize_to_limit;
    double final_gain_db;
    uint32_t output_bit_depth;
    uint32_t opus_bitrate_per_ch_kbps;
    uint32_t apac_bitrate_kbps;
    uint32_t apac_drc_music;
    uint32_t apac_caf;
    uint32_t has_sofa;
    MradmCapiText sofa;
    uint32_t has_policy_path;
    MradmCapiText policy_path;
    uint32_t has_policy_json;
    MradmCapiText policy_json;
    uint32_t has_matrix_path;
    MradmCapiText matrix_path;
    uint32_t has_matrix_json;
    MradmCapiText matrix_json;
    uint32_t has_report_path;
    MradmCapiText report_path;
    uint32_t capture_report;
    uint32_t default_interp_ms;
    uint32_t object_smoothing_frames;
    uint32_t speaker_geometry;
    uint32_t routing;
    uint32_t speaker_spread;
    uint32_t triple_balance_mode;
    uint32_t binaural_spread;
    uint32_t lfe_routing;
    uint32_t apple_speaker_rendering_flags;
    double render_start_sec;
    uint32_t has_end;
    double render_end_sec;
    uint32_t iamf_mp4;
    const MradmCapiText* iamf_layers;
    size_t iamf_layers_len;
    void* room_profile;
    MradmCapiText room_hrir;
    double room_metres;
    uint32_t room_quality;
    uint32_t has_reference;
    MradmCapiScreen reference;
    uint32_t has_reproduction;
    MradmCapiScreen reproduction;
} MradmCapiOptions;

typedef struct MradmCapiOptionsLists MradmCapiOptionsLists;

/* Fills *out with views of opts (alive until mradm_capi_options_release of the returned lists); NULL for
   NULL arguments or when out of memory. The caller owns out->room_profile. */
MradmCapiOptionsLists* mradm_capi_options(const adm_render_options_t* opts, MradmCapiOptions* out);
void mradm_capi_options_release(MradmCapiOptionsLists* lists);

#ifdef __cplusplus
}
#endif
