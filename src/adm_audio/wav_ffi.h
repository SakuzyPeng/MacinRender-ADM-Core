// The records of the Rust WAV services (rust_services_ffi.h). Paths are UTF-8; C++ callers convert
// native Windows paths before crossing this boundary.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MradmWavReader MradmWavReader;
// Final float PCM output: caf != 0 writes little-endian float32 CAF with the layout's channel layout
// tag, otherwise float32 RF64 WAVE. All writers number their writer/pass* checkpoints from one
// process-wide sequence. Destroy finishes an unfinished writer on a best-effort basis; repeated
// finish calls return the first result.
typedef struct MradmOutputWriter MradmOutputWriter;
// Fields are shared with Rust, including queries performed by Rust FFI tests.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmWavInfo {
    uint64_t frames;
    uint32_t channels;
    uint32_t rate;
    uint32_t channel_mask; // 0 without WAVE_FORMAT_EXTENSIBLE
    uint16_t bits;         // container bits per sample
    uint16_t format;       // 1 = PCM, 3 = IEEE float
} MradmWavInfo;
// cppcheck-suppress-end unusedStructMember

#ifdef __cplusplus
}
#endif
