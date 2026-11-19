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

// bext fields (wav_append_bext, which also writes the HOA3 ambi marker when requested and updates the
// RIFF size or ds64.bw64Size). Byte strings are borrowed; a null pointer is valid only with length 0.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmWavBext {
    const uint8_t* description;
    size_t description_len;
    const uint8_t* originator;
    size_t originator_len;
    const uint8_t* originator_reference;
    size_t originator_reference_len;
    const uint8_t* date_utc; // ISO 8601 UTC
    size_t date_utc_len;
    double loudness;
    double true_peak;
    uint8_t has_loudness;
    uint8_t has_true_peak;
    uint8_t hoa3_ambi; // also write the third-order AmbiX ambi marker
} MradmWavBext;
// cppcheck-suppress-end unusedStructMember
#ifdef __cplusplus
}
#endif
