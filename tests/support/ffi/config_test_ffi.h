// The part of the private boundary in src/adm_core/config_ffi.h that production C/C++
// does not use: tests and consistency tools call these mradm-ffi entries (some are also reached
// from Rust inside the workspace). Production keeps only what it calls.
//
// DirectSpeakers matrix and screen configuration JSON in Rust mradm-scene.
// A matrix view borrows from its handle until mradm_scene_direct_matrix_destroy. Status values
// follow adm::ErrorCode.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_core/config_ffi.h"
#include "adm_test_ffi.h"
#include "room_test_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmDirectMatrix MradmDirectMatrix;

// cppcheck-suppress-begin unusedStructMember
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
