// Private Rust WAV boundary; buffers are borrowed for one call, owners have matching destroy functions.
// Paths are UTF-8; C++ callers convert native Windows paths before crossing this boundary.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MradmWavReader MradmWavReader;
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
int32_t mradm_wav_reader_open(const uint8_t*, size_t, MradmWavReader**, MradmWavInfo*, uint8_t*, size_t);
// Validate native/Ogg FLAC completely and count samples, including unknown-length streams.
int32_t mradm_flac_validate(const uint8_t*, size_t, MradmWavInfo*, uint8_t*, size_t);
void mradm_wav_reader_destroy(MradmWavReader*);
int32_t mradm_wav_reader_read(MradmWavReader*, float*, size_t, uint64_t*, uint8_t*, size_t);
int32_t mradm_wav_reader_seek(MradmWavReader*, uint64_t, uint8_t*, size_t);
// Final float PCM output: caf != 0 writes little-endian float32 CAF with the layout's channel layout
// tag, otherwise float32 RF64 WAVE. All writers number their writer/pass* checkpoints from one
// process-wide sequence. Destroy finishes an unfinished writer on a best-effort basis; repeated
// finish calls return the first result.
int32_t mradm_output_writer_create(
    const uint8_t*, size_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t, MradmOutputWriter**, uint8_t*, size_t);
void mradm_output_writer_destroy(MradmOutputWriter*);
int32_t mradm_output_writer_write(MradmOutputWriter*, const float*, size_t, uint8_t*, size_t);
int32_t mradm_output_writer_finish(MradmOutputWriter*, uint8_t*, size_t);
// Appends an info chunk with the non-empty encoder, date and comments values.
int32_t mradm_caf_append_info(
    const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t);

// Container-level edits. Byte strings are borrowed for one call; a null pointer is valid only with length 0.
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
// Appends bext (and the HOA3 ambi marker when requested) and updates the RIFF size or ds64.bw64Size.
int32_t mradm_wav_append_bext(const uint8_t*, size_t, const MradmWavBext*, uint8_t*, size_t);
#ifdef __cplusplus
}
#endif
