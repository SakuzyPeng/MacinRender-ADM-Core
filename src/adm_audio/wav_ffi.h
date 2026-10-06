// Private Rust WAV boundary; buffers are borrowed for one call, owners have matching destroy functions.
// Paths are UTF-8; C++ callers convert native Windows paths before crossing this boundary.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MradmWavReader MradmWavReader;
typedef struct MradmWavWriter MradmWavWriter;
typedef struct MradmWavLayout MradmWavLayout;
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
void mradm_wav_reader_destroy(MradmWavReader*);
int32_t mradm_wav_reader_read(MradmWavReader*, float*, size_t, uint64_t*, uint8_t*, size_t);
int32_t mradm_wav_reader_seek(MradmWavReader*, uint64_t, uint8_t*, size_t);
// float_output selects float32 RF64 (bits must be 32); exclusive refuses to replace an existing file.
int32_t mradm_wav_writer_create(
    const uint8_t*, size_t, uint32_t, uint32_t, uint16_t, uint8_t, uint8_t, MradmWavWriter**, uint8_t*, size_t);
void mradm_wav_writer_destroy(MradmWavWriter*);
int32_t mradm_wav_writer_write(MradmWavWriter*, const float*, size_t, uint8_t*, size_t);
int32_t mradm_wav_writer_finish(MradmWavWriter*, uint8_t*, size_t);

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
typedef struct MradmWavChnaTrack {
    const uint8_t* uid;
    size_t uid_len;
    const uint8_t* track_format;
    size_t track_format_len;
    const uint8_t* pack_format;
    size_t pack_format_len;
    uint16_t track_index; // one-based
} MradmWavChnaTrack;
typedef struct MradmWavLayoutOptions {
    const uint16_t* permutation; // file channel -> source channel; empty is identity
    size_t permutation_len;
    const MradmWavChnaTrack* chna;
    size_t chna_len;
    const uint8_t* axml;
    size_t axml_len;
    uint32_t channel_mask;
    uint8_t force_extensible;
    uint8_t prefer_riff;
    uint8_t include_pcm_fact;
} MradmWavLayoutOptions;
// cppcheck-suppress-end unusedStructMember
// Appends bext (and the HOA3 ambi marker when requested) and updates the RIFF size or ds64.bw64Size.
int32_t mradm_wav_append_bext(const uint8_t*, size_t, const MradmWavBext*, uint8_t*, size_t);
// Layout rewrite into an exclusively created target: begin writes the header, step copies frames
// (0 once done), finish writes trailing chunks. The caller owns removal and installation of the target.
int32_t mradm_wav_layout_begin(const uint8_t*,
                               size_t,
                               const uint8_t*,
                               size_t,
                               const MradmWavLayoutOptions*,
                               MradmWavLayout**,
                               uint64_t*,
                               uint8_t*,
                               size_t);
int32_t mradm_wav_layout_step(MradmWavLayout*, uint64_t, uint64_t*, uint8_t*, size_t);
int32_t mradm_wav_layout_finish(MradmWavLayout*, uint8_t*, size_t);
void mradm_wav_layout_destroy(MradmWavLayout*);
// Copies source into a created/truncated target with the axml payload replaced; PCM bytes are copied verbatim.
int32_t
mradm_wav_replace_axml(const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t);
// First chunk with a four-byte id. Capacity 0 only reports found/size; otherwise it must equal the size.
int32_t
mradm_wav_reader_chunk(MradmWavReader*, const uint8_t*, uint8_t*, size_t, uint64_t*, uint8_t*, uint8_t*, size_t);
// Import-side CHNA: stored track indices and 12-byte UIDs (uid holds 12 * capacity bytes).
// Capacity 0 only reports found/count; otherwise it must be at least count.
int32_t mradm_wav_reader_chna(MradmWavReader*, uint16_t*, uint8_t*, size_t, size_t*, uint8_t*, uint8_t*, size_t);
// Tolerant routing probe for a top-level chunk; fails only for unreadable or non-RIFF/RF64/BW64 WAVE files.
int32_t mradm_wav_has_chunk(const uint8_t*, size_t, const uint8_t*, uint8_t*, uint8_t*, size_t);
#ifdef __cplusplus
}
#endif
