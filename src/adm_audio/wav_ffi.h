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
#ifdef __cplusplus
}
#endif
