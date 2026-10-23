// DirectSpeakers matrix and screen configuration JSON in Rust mradm-scene (PCM consistency 4.4).
// A matrix view borrows from its handle until mradm_scene_direct_matrix_destroy. Status values
// follow adm::ErrorCode.
#pragma once
#include "../adm_dsp/room_ffi.h"
#include "../adm_metadata/adm_ffi.h"
#ifdef __cplusplus
extern "C" {
#endif
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDirectMatrix MradmDirectMatrix;

typedef struct MradmDirectMatrixTarget {
    MradmAdmText label;
    float weight;
    float gain;
} MradmDirectMatrixTarget;

typedef struct MradmDirectMatrixRoute {
    MradmAdmText source_label;
    uint32_t mute;
    const MradmDirectMatrixTarget* targets;
    size_t targets_len;
} MradmDirectMatrixRoute;

typedef struct MradmDirectMatrixView {
    MradmAdmText output_layout;
    const MradmDirectMatrixRoute* routes;
    size_t routes_len;
} MradmDirectMatrixView;

typedef struct MradmScreenConfigView {
    uint32_t has_reference;
    uint32_t has_reproduction;
    MradmRoomScreenGeometry reference;
    MradmRoomScreenGeometry reproduction;
} MradmScreenConfigView;
// cppcheck-suppress-end unusedStructMember

// The caller attaches its source label to errors. For both parsers, message_len is the buffer
// capacity on input and bytes copied (excluding the final terminator) on output. NULL disables it.
int32_t mradm_scene_direct_matrix_parse(const uint8_t* json,
                                        size_t json_len,
                                        MradmDirectMatrix** output,
                                        MradmDirectMatrixView* view,
                                        char* message,
                                        size_t* message_len);
void mradm_scene_direct_matrix_destroy(MradmDirectMatrix* handle);
// Parses without validating the geometry (mradm_room_validate_screens does that).
int32_t mradm_scene_screen_parse(
    const uint8_t* json, size_t json_len, MradmScreenConfigView* output, char* message, size_t* message_len);

#ifdef __cplusplus
}
#endif
