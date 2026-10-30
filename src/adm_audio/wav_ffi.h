// Private Rust WAV boundary; buffers are borrowed for one call, owners have matching destroy functions.
// Paths are UTF-8; C++ callers convert native Windows paths before crossing this boundary.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MradmWavReader MradmWavReader;
typedef struct MradmWavChunks MradmWavChunks;
typedef struct MradmWavWriter MradmWavWriter;
typedef struct MradmOutputPost MradmOutputPost;
typedef struct MradmOutputWriter MradmOutputWriter;
typedef struct MradmCafReader MradmCafReader;
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
// Final float PCM output: caf != 0 writes little-endian float32 CAF with the layout's channel layout
// tag, otherwise float32 RF64 WAVE. All writers number their writer/pass* checkpoints from one
// process-wide sequence. Destroy finishes an unfinished writer on a best-effort basis; repeated
// finish calls return the first result.
int32_t mradm_output_writer_create(
    const uint8_t*, size_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t, MradmOutputWriter**, uint8_t*, size_t);
void mradm_output_writer_destroy(MradmOutputWriter*);
int32_t mradm_output_writer_write(MradmOutputWriter*, const float*, size_t, uint8_t*, size_t);
int32_t mradm_output_writer_finish(MradmOutputWriter*, uint8_t*, size_t);
// float32 little-endian CAF; info reports frames, channels and rate (bits 32, format 3).
int32_t mradm_caf_reader_open(const uint8_t*, size_t, MradmCafReader**, MradmWavInfo*, uint8_t*, size_t);
void mradm_caf_reader_destroy(MradmCafReader*);
int32_t mradm_caf_reader_read(MradmCafReader*, float*, size_t, uint64_t*, uint8_t*, size_t);
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
// One post-processing progress report; message is static text. emit asks for a progress event;
// once complete the data is written and finish installs the output. install_check (begin only)
// asks for one more cancellation check before finish.
typedef struct MradmOutputPostProgress {
    double fraction;
    uint64_t done;
    uint64_t total;
    const uint8_t* message;
    size_t message_len;
    uint8_t emit;
    uint8_t complete;
    uint8_t install_check;
} MradmOutputPostProgress;
// cppcheck-suppress-end unusedStructMember
// Appends bext (and the HOA3 ambi marker when requested) and updates the RIFF size or ds64.bw64Size.
int32_t mradm_wav_append_bext(const uint8_t*, size_t, const MradmWavBext*, uint8_t*, size_t);
// Post-processing passes over a rendered file, each written to a sidecar and installed in place.
// Errors fill message and context. Begin returns a null pass with the final event when the pass
// is skipped. The caller checks cancellation before each step (and before finish when asked),
// then steps until complete and finishes; destroying an unfinished pass removes its sidecar.
int32_t mradm_output_gain_begin(const uint8_t*,
                                size_t,
                                float,
                                const uint8_t*,
                                size_t,
                                uint8_t,
                                MradmOutputPost**,
                                MradmOutputPostProgress*,
                                uint8_t*,
                                size_t,
                                uint8_t*,
                                size_t);
int32_t mradm_output_trim_begin(const uint8_t*,
                                size_t,
                                uint64_t,
                                uint64_t,
                                const uint8_t*,
                                size_t,
                                MradmOutputPost**,
                                MradmOutputPostProgress*,
                                uint8_t*,
                                size_t,
                                uint8_t*,
                                size_t);
int32_t mradm_output_bitdepth_begin(
    const uint8_t*, size_t, uint16_t, MradmOutputPost**, MradmOutputPostProgress*, uint8_t*, size_t, uint8_t*, size_t);
// With options the layout is explicit; otherwise it follows the layout id and profile
// (0 ADM semantics, 1 speaker re-render) and the cancelled flag is checked after building it.
int32_t mradm_output_finalize_begin(const uint8_t*,
                                    size_t,
                                    const uint8_t*,
                                    size_t,
                                    uint32_t,
                                    const MradmWavLayoutOptions*,
                                    uint8_t,
                                    MradmOutputPost**,
                                    MradmOutputPostProgress*,
                                    uint8_t*,
                                    size_t,
                                    uint8_t*,
                                    size_t);
int32_t mradm_output_post_step(MradmOutputPost*, MradmOutputPostProgress*, uint8_t*, size_t, uint8_t*, size_t);
int32_t mradm_output_post_finish(MradmOutputPost*, MradmOutputPostProgress*, uint8_t*, size_t, uint8_t*, size_t);
void mradm_output_post_destroy(MradmOutputPost*);
// Target becomes a copy of source with the axml payload replaced (PCM bytes verbatim), written to a
// sidecar and installed; aliases of the source are safe. On failure the target is left unchanged.
int32_t mradm_wav_replace_axml(
    const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t, uint8_t*, size_t);
// Metadata-only reader: strict chunk bounds, independent of sample encoding.
int32_t mradm_wav_chunks_open(const uint8_t*, size_t, MradmWavChunks**, uint8_t*, size_t);
void mradm_wav_chunks_destroy(MradmWavChunks*);
// First chunk with a four-byte id. Capacity 0 only reports found/size; otherwise it must equal the size.
int32_t mradm_wav_chunks_read(MradmWavChunks*, const uint8_t*, uint8_t*, size_t, uint64_t*, uint8_t*, uint8_t*, size_t);
// Import-side CHNA: stored track indices and 12-byte UIDs (uid holds 12 * capacity bytes).
// Capacity 0 only reports found/count; otherwise it must be at least count.
int32_t mradm_wav_chunks_chna(MradmWavChunks*, uint16_t*, uint8_t*, size_t, size_t*, uint8_t*, uint8_t*, size_t);
// Tolerant routing probe for a top-level chunk; fails only for unreadable or non-RIFF/RF64/BW64 WAVE files.
int32_t mradm_wav_has_chunk(const uint8_t*, size_t, const uint8_t*, uint8_t*, uint8_t*, size_t);
#ifdef __cplusplus
}
#endif
