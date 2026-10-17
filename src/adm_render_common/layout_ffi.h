// Private loudspeaker layout boundary (PCM consistency 4.3); see mradm-scene `layouts`. The table
// points at process-lifetime Rust data and is never freed. Status values follow adm::ErrorCode.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmLayoutSpeaker {
    const uint8_t* label;
    size_t label_len;
    float azimuth;
    float elevation;
    uint32_t is_lfe;
    uint32_t has_azimuth_range;
    float azimuth_range[2];
    uint32_t has_elevation_range;
    float elevation_range[2];
} MradmLayoutSpeaker;

typedef struct MradmLayoutEntry {
    const uint8_t* id;
    size_t id_len;
    const uint8_t* display_name;
    size_t display_name_len;
    const MradmLayoutSpeaker* speakers;
    size_t speakers_len;
} MradmLayoutEntry;

// geometry: 0 standard ADM nominal positions, 1 Apple effective positions.
int32_t mradm_scene_layouts(
    uint32_t geometry, const MradmLayoutEntry** entries, size_t* entries_len, uint8_t* message, size_t capacity);

#ifdef __cplusplus
}
#endif
