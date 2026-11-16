// The part of the private boundary in src/adm_dsp/dsp_ffi.h that production C/C++
// does not use: tests and consistency tools call these mradm-ffi entries (some are also reached
// from Rust inside the workspace). Production keeps only what it calls.
//
// Private Rust boundary. Buffers must be valid, aligned, non-overlapping and
// live for the call. Mutable handles require exclusive access. Status values
// follow adm::ErrorCode; the optional message buffer is caller-owned UTF-8.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../../src/adm_dsp/dsp_ffi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Prepared PCM timelines; fields contain only project-owned scalar data.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspMixRow {
    size_t input_channel, block_offset, block_count;
    float output_gain;
} MradmDspMixRow;
typedef struct MradmDspMixBlock {
    uint64_t start, end, interpolation;
    uint32_t flags; // jump=1, object smoothing=2, explicit interpolation=4
} MradmDspMixBlock;
// cppcheck-suppress-end unusedStructMember
int mradm_dsp_mix_plan_create(size_t inputs,
                              size_t outputs,
                              const MradmDspMixRow* rows,
                              size_t row_count,
                              const MradmDspMixBlock* blocks,
                              size_t block_count,
                              const float* gains_f32,
                              size_t len_f32,
                              const double* gains_f64,
                              size_t len_f64,
                              uint32_t kind,
                              void** out,
                              char* message,
                              size_t capacity);
void mradm_dsp_mix_plan_destroy(void* plan);
int mradm_dsp_mix_create(const void* plan,
                         size_t max_frames,
                         uint64_t interpolation,
                         uint32_t smoothing,
                         void** out,
                         char* message,
                         size_t capacity);
int mradm_dsp_mix_dynamic_create(size_t inputs,
                                 size_t outputs,
                                 const size_t* channels,
                                 size_t rows,
                                 size_t block_capacity,
                                 size_t max_frames,
                                 uint64_t interpolation,
                                 void** out,
                                 char* message,
                                 size_t capacity);
void mradm_dsp_mix_destroy(void* mixer);
int mradm_dsp_mix_reset(void* mixer, char* message, size_t capacity);
int mradm_dsp_mix_update(void* mixer,
                         size_t row,
                         const MradmDspMixBlock* blocks,
                         size_t block_count,
                         const float* gains,
                         size_t gain_count,
                         float output_gain,
                         char* message,
                         size_t capacity);
// SIZE_MAX selects all rows (with configured object smoothing); a single row has no extra smoothing.
int mradm_dsp_mix_speaker(void* mixer,
                          size_t row,
                          const float* pcm,
                          size_t pcm_len,
                          float* out,
                          size_t out_len,
                          const float* live,
                          size_t live_len,
                          uint64_t start,
                          size_t frames,
                          uint32_t override_gain,
                          float gain,
                          char* message,
                          size_t capacity);
int mradm_dsp_mix_ear(void* mixer,
                      const float* pcm,
                      size_t pcm_len,
                      float* direct,
                      size_t direct_len,
                      float* diffuse,
                      size_t diffuse_len,
                      uint64_t start,
                      size_t frames,
                      char* message,
                      size_t capacity);
int mradm_dsp_matrix_create(
    size_t inputs, size_t outputs, const float* gains, size_t length, void** out, char* message, size_t capacity);
void mradm_dsp_matrix_destroy(void* matrix);
int mradm_dsp_matrix_process(const void* matrix,
                             const float* pcm,
                             size_t pcm_len,
                             float* out,
                             size_t out_len,
                             size_t frames,
                             char* message,
                             size_t capacity);

// HpTF coefficients, text import and the realtime processor. Coefficients are
// copied explicitly; these private PODs do not alter the public C ABI.
// All audio lengths count interleaved floats. Empty audio may be null.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspHptfBand {
    uint32_t kind;
    uint32_t enabled;
    double frequency;
    double gain_db;
    double q;
} MradmDspHptfBand;
typedef struct MradmDspHptfBiquad {
    float b0, b1, b2, a1, a2;
} MradmDspHptfBiquad;
typedef struct MradmDspHptfCoefficients {
    uint32_t sample_rate;
    uint32_t band_count;
    float preamp_gain;
    MradmDspHptfBiquad sections[32];
    float max_response_db;
    float auto_trim_db;
    float preamp_db;
} MradmDspHptfCoefficients;
typedef struct MradmDspHptfSnapshot {
    MradmDspHptfCoefficients coefficients;
    uint64_t revision;
    MradmDspHptfCoefficients right;
    uint32_t independent;
} MradmDspHptfSnapshot;
// AutoEq text import. error: 0 parsed, 1 Preamp value, 2 Fc value, 3 Gain/Q value, 4 nothing usable;
// line_offset/line_len index the offending trimmed line of the caller's text.
typedef struct MradmDspHptfParseResult {
    uint32_t error;
    double preamp_db;
    size_t band_count;
    size_t line_offset;
    size_t line_len;
} MradmDspHptfParseResult;
// cppcheck-suppress-end unusedStructMember
// Writes at most capacity bands; band_count is always the full count (retry when larger).
int32_t mradm_dsp_hptf_parse(const uint8_t* text,
                             size_t length,
                             MradmDspHptfBand* bands,
                             size_t capacity,
                             MradmDspHptfParseResult* result,
                             char* message,
                             size_t message_capacity);
// The shared HpTF processor (MonitorEngine's insert; the Scene output owns its own): control
// threads publish designed targets, the audio callback processes and reports what it applied.
// A target the processor would reject fails on publication. Reset only while the callback is
// parked; snapshot reports the newest applied target.
int32_t mradm_dsp_hptf_live_create(size_t channels, uint32_t rate, void** output, char* message, size_t capacity);
void mradm_dsp_hptf_live_destroy(void* handle);
int32_t
mradm_dsp_hptf_live_publish(const void* handle, const MradmDspHptfSnapshot* target, char* message, size_t capacity);
int32_t mradm_dsp_hptf_live_bypass(const void* handle, uint64_t revision, char* message, size_t capacity);
void mradm_dsp_hptf_live_reset(const void* handle);
void mradm_dsp_hptf_live_process(const void* handle, float* samples, size_t length);
void mradm_dsp_hptf_live_snapshot(const void* handle, MradmDspHptfSnapshot* output);

// Owned, sample-rate-independent profiles. source_kind matches the four public configs:
// 0 shared file, 1 shared parameters, 2 stereo files, 3 stereo parameters.
int32_t
mradm_dsp_hptf_settings_create(const void* source, uint32_t source_kind, void** output, char* message, size_t capacity);
void mradm_dsp_hptf_settings_destroy(void* settings);
int32_t mradm_dsp_hptf_settings_design(
    const void* settings, uint32_t rate, MradmDspHptfSnapshot* output, char* message, size_t capacity);
int32_t mradm_dsp_hptf_validate(
    const MradmDspHptfBand* bands, size_t count, double preamp, size_t* invalid_band, char* message, size_t capacity);

int32_t mradm_dsp_fft_create(size_t length, void** output, char* message, size_t capacity);
void mradm_dsp_fft_destroy(void* handle);
int32_t mradm_dsp_fft_forward(void* handle,
                              const float* input,
                              size_t input_length,
                              float* output,
                              size_t output_length,
                              char* message,
                              size_t capacity);
int32_t mradm_dsp_fft_inverse(void* handle,
                              const float* input,
                              size_t input_length,
                              float* output,
                              size_t output_length,
                              char* message,
                              size_t capacity);

int32_t mradm_dsp_panner_create(
    const float* directions, size_t length, int32_t is_3d, void** output, char* message, size_t capacity);
void mradm_dsp_panner_destroy(void* handle);
// One live VBAP point source: x/y/z when cartesian != 0, otherwise azimuth/elevation/distance.
// Fields are consumed by the Rust panner across this private boundary.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmPannerSource {
    uint32_t cartesian;
    float position[3];
    float gain;
    float width;
    float height;
    float depth;
} MradmPannerSource;
// cppcheck-suppress-end unusedStructMember
int32_t mradm_dsp_panner_sources(const void* handle,
                                 const MradmPannerSource* sources,
                                 size_t count,
                                 uint32_t spread,
                                 uint32_t sum,
                                 float* scratch,
                                 float* output,
                                 size_t length,
                                 char* message,
                                 size_t capacity);
int32_t mradm_dsp_panner_gains(const void* handle,
                               float azimuth,
                               float elevation,
                               float spread,
                               float* output,
                               size_t length,
                               char* message,
                               size_t capacity);

#ifdef __cplusplus
}
#endif
