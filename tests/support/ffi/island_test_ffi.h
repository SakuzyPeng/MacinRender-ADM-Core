// The Rust services only the island tests call (MradmRustServices::tests): the routed scene import
// (the Apple island and smoke tests) and the DirectSpeakers matrix parser (the Apple matrix test).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_core/config_ffi.h"
#include "../../../src/adm_core/rust_services_ffi.h"
#include "../../../src/adm_metadata/adm_ffi.h"
#include "../../../src/adm_metadata/scene_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmDirectMatrix MradmDirectMatrix;

struct MradmRustTestServices {
    // routed: 0 = ADM BW64 only; 1 = ADM or channel bed by the options. path is UTF-8. `context`
    // receives the error locator: 0 none, 1 "input=<path>", 2 "path=<path>", 3 the caller's label.
    int32_t (*scene_import)(const uint8_t* path,
                            size_t path_len,
                            uint32_t routed,
                            uint32_t has_layout,
                            MradmAdmText layout,
                            const MradmAdmText* input_labels,
                            size_t input_labels_len,
                            MradmSceneHandle** output,
                            uint32_t* context,
                            uint8_t* message,
                            size_t capacity);
    // A matrix view borrows from its handle until scene_direct_matrix_destroy. message_len is the
    // buffer capacity on input and the bytes copied (without the terminator) on output.
    int32_t (*scene_direct_matrix_parse)(const uint8_t* json,
                                         size_t json_len,
                                         MradmDirectMatrix** output,
                                         MradmDirectMatrixView* view,
                                         char* message,
                                         size_t* message_len);
    void (*scene_direct_matrix_destroy)(MradmDirectMatrix* handle);
};

#ifdef __cplusplus
}
#endif
