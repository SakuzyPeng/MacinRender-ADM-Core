// Private ADM metadata boundary. Views are borrowed until the matching handle is destroyed.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmAdmSpan {
    size_t offset;
    size_t len;
} MradmAdmSpan;

typedef struct MradmAdmPosition {
    uint32_t cartesian;
    float azimuth;
    float elevation;
    float distance;
    float x;
    float y;
    float z;
} MradmAdmPosition;

typedef struct MradmAdmGain {
    uint32_t present;
    uint32_t decibels;
    double value;
    float linear;
} MradmAdmGain;

typedef struct MradmAdmBlockSource {
    MradmAdmGain gain;
    uint32_t rtime_present;
    uint64_t rtime_samples;
    uint32_t duration_present;
    uint64_t duration_samples;
} MradmAdmBlockSource;

typedef struct MradmAdmLoudness {
    uint32_t present;
    uint32_t fields;
    float integrated;
    float true_peak;
    float range;
    float momentary;
    float short_term;
    float dialogue;
    uint32_t method;
} MradmAdmLoudness;

typedef struct MradmAdmProgramme {
    uint32_t id;
    uint32_t name;
    uint32_t language;
    MradmAdmSpan content_ids;
    MradmAdmSpan labels;
    uint64_t start;
    uint32_t end_present;
    uint64_t end;
    MradmAdmLoudness loudness;
    uint32_t reference_screen;
} MradmAdmProgramme;

typedef struct MradmAdmContent {
    uint32_t id;
    uint32_t name;
    uint32_t language;
    MradmAdmSpan object_ids;
    MradmAdmSpan labels;
    MradmAdmLoudness loudness;
    uint32_t dialogue_kind;
    uint32_t content_kind;
} MradmAdmContent;

typedef struct MradmAdmObject {
    uint32_t node;
    uint32_t id;
    uint32_t name;
    float gain;
    uint32_t mute;
    uint32_t head_locked;
    uint64_t end;
    uint32_t offset_present;
    MradmAdmPosition position_offset;
    MradmAdmSpan tracks;
    MradmAdmSpan labels;
    uint32_t importance_present;
    int32_t importance;
    uint32_t dialogue_present;
    uint32_t dialogue;
    MradmAdmGain source_gain;
    uint32_t mute_present;
    uint32_t start_present;
    uint64_t start;
    uint64_t absolute_start;
    uint32_t duration_present;
    uint64_t duration;
    MradmAdmSpan child_objects;
    uint32_t has_parent;
} MradmAdmObject;

typedef struct MradmAdmTrack {
    uint32_t uid;
    int32_t channel;
    MradmAdmSpan blocks;
    MradmAdmSpan ds_blocks;
} MradmAdmTrack;

typedef struct MradmAdmObjectBlock {
    uint32_t node;
    MradmAdmPosition position;
    float gain;
    float diffuse;
    float width;
    float height;
    float depth;
    uint64_t start;
    uint64_t end;
    uint32_t jump;
    uint32_t interpolation_present;
    uint64_t interpolation;
    uint32_t channel_lock;
    uint32_t max_distance_present;
    float max_distance;
    float divergence;
    float azimuth_range;
    float position_range;
    uint32_t screen_ref;
    uint32_t head_locked;
    MradmAdmBlockSource source;
} MradmAdmObjectBlock;

typedef struct MradmAdmDirectBlock {
    uint32_t node;
    MradmAdmSpan labels;
    uint32_t pack_id;
    uint32_t has_position;
    MradmAdmPosition position;
    float azimuth;
    float elevation;
    float distance;
    uint32_t bounds_present;
    float azimuth_min;
    float azimuth_max;
    float elevation_min;
    float elevation_max;
    float distance_min;
    float distance_max;
    float gain;
    uint32_t low_pass_present;
    float low_pass;
    uint64_t start;
    uint64_t end;
    uint32_t head_locked;
    MradmAdmBlockSource source;
} MradmAdmDirectBlock;

typedef struct MradmAdmHoa {
    uint32_t object_id;
    uint32_t pack_id;
    uint32_t normalization;
    double nfc_ref_dist;
    uint32_t screen_ref;
    float gain;
    uint32_t mute;
    uint32_t head_locked;
    uint64_t start;
    uint64_t end;
    MradmAdmSpan channels;
} MradmAdmHoa;

typedef struct MradmAdmHoaChannel {
    uint32_t uid;
    int32_t channel;
    int32_t order;
    int32_t degree;
    MradmAdmSpan blocks;
} MradmAdmHoaChannel;

typedef struct MradmAdmHoaBlock {
    uint32_t node;
    float gain;
    uint32_t head_locked;
    uint64_t start;
    uint64_t end;
} MradmAdmHoaBlock;

typedef struct MradmAdmPatch {
    uint32_t node;
    uint32_t field;
    uint32_t present;
    uint32_t original_present;
    double value;
    double original;
    uint64_t samples;
    uint64_t original_samples;
} MradmAdmPatch;

typedef struct MradmAdmSpeaker {
    uint32_t label;
    float azimuth;
    float elevation;
    uint32_t lfe;
    uint32_t ranges;
    float azimuth_min;
    float azimuth_max;
    float elevation_min;
    float elevation_max;
} MradmAdmSpeaker;

typedef struct MradmAdmText {
    const uint8_t* data;
    size_t len;
} MradmAdmText;
typedef struct MradmAdmView {
    const MradmAdmText* strings;
    size_t strings_len;
    const uint32_t* indices;
    size_t indices_len;
    const MradmAdmProgramme* programmes;
    size_t programmes_len;
    const MradmAdmContent* contents;
    size_t contents_len;
    const MradmAdmObject* objects;
    size_t objects_len;
    const MradmAdmTrack* tracks;
    size_t tracks_len;
    const MradmAdmObjectBlock* object_blocks;
    size_t object_blocks_len;
    const MradmAdmDirectBlock* direct_blocks;
    size_t direct_blocks_len;
    const MradmAdmHoa* hoa;
    size_t hoa_len;
    const MradmAdmHoaChannel* hoa_channels;
    size_t hoa_channels_len;
    const MradmAdmHoaBlock* hoa_blocks;
    size_t hoa_blocks_len;
    const uint32_t* warnings;
    size_t warnings_len;
} MradmAdmView;
typedef struct MradmAdmHandle MradmAdmHandle;
typedef struct MradmAdmBuffer MradmAdmBuffer;
typedef struct MradmAdmChna {
    MradmAdmText uid;
    uint16_t channel;
} MradmAdmChna;
typedef struct MradmAdmOutputChna {
    uint16_t track;
    MradmAdmText uid;
    MradmAdmText format;
    MradmAdmText pack;
} MradmAdmOutputChna;
int32_t mradm_adm_parse(const uint8_t* xml,
                        size_t xml_len,
                        uint32_t sample_rate,
                        const MradmAdmChna* chna,
                        size_t chna_len,
                        MradmAdmHandle** output,
                        uint8_t* error,
                        size_t capacity);
int32_t mradm_adm_view(const MradmAdmHandle* handle, MradmAdmView* output, uint8_t* error, size_t capacity);
void mradm_adm_destroy(MradmAdmHandle* handle);
int32_t mradm_adm_patch(const MradmAdmHandle* handle,
                        const MradmAdmPatch* patches,
                        size_t length,
                        MradmAdmBuffer** output,
                        uint8_t* error,
                        size_t capacity);
int32_t mradm_adm_generate(uint32_t kind,
                           MradmAdmText name,
                           const MradmAdmSpeaker* speakers,
                           const MradmAdmText* labels,
                           size_t count,
                           MradmAdmBuffer** output,
                           uint8_t* error,
                           size_t capacity);
int32_t mradm_adm_buffer_view(const MradmAdmBuffer* handle,
                              MradmAdmText* xml,
                              const MradmAdmOutputChna** chna,
                              size_t* length,
                              uint8_t* error,
                              size_t capacity);
void mradm_adm_buffer_destroy(MradmAdmBuffer* handle);
#ifdef __cplusplus
}
#endif
