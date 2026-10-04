#pragma once

// Private Rust boundary. Buffers must be valid, aligned, non-overlapping and
// live for the call. Mutable handles require exclusive access. Status values
// follow adm::ErrorCode; the optional message buffer is caller-owned UTF-8.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Live gain banks and stereo device protection. Lengths count floats; empty buffers may be null.
int mradm_dsp_gain_create(
    size_t channels, uint32_t rate, uint32_t ramp_ms, void** output, char* message, size_t capacity);
void mradm_dsp_gain_destroy(void* handle);
int mradm_dsp_gain_reset(void* handle, char* message, size_t capacity);
int mradm_dsp_gain_set_targets(void* handle, const float* targets, size_t length, char* message, size_t capacity);
// mode 0 fills gain envelopes, mode 1 multiplies PCM in place.
int mradm_dsp_gain_process(void* handle, float* data, size_t length, uint32_t mode, char* message, size_t capacity);
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspPeakStatus {
    size_t lookahead, buffered, writable, readable;
} MradmDspPeakStatus;
// cppcheck-suppress-end unusedStructMember
int mradm_dsp_peak_guard_create(uint32_t rate, void** output, char* message, size_t capacity);
void mradm_dsp_peak_guard_destroy(void* handle);
int mradm_dsp_peak_guard_reset(void* handle, char* message, size_t capacity);
int mradm_dsp_peak_guard_status(
    const void* handle, uint32_t ended, MradmDspPeakStatus* status, char* message, size_t capacity);
int mradm_dsp_peak_guard_push(void* handle, const float* data, size_t length, char* message, size_t capacity);
int mradm_dsp_peak_guard_pop(void* handle,
                             float* data,
                             size_t length,
                             float volume,
                             uint32_t ended,
                             size_t* frames,
                             char* message,
                             size_t capacity);

// HpTF control-thread design and prepared realtime kernels. Coefficients are
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
} MradmDspHptfSnapshot;
typedef struct MradmDspHptfUpdate {
    MradmDspHptfSnapshot snapshot;
    uint32_t blending;
    uint32_t applied;
} MradmDspHptfUpdate;
// cppcheck-suppress-end unusedStructMember
int32_t mradm_dsp_hptf_design(const MradmDspHptfBand* bands,
                              size_t length,
                              double preamp_db,
                              uint32_t rate,
                              uint32_t mode,
                              MradmDspHptfCoefficients* output,
                              char* message,
                              size_t capacity);
int32_t mradm_dsp_hptf_magnitude(
    const MradmDspHptfCoefficients* coefficients, double hz, double* output, char* message, size_t capacity);
int32_t mradm_dsp_hptf_cascade_create(size_t channels, void** output, char* message, size_t capacity);
int32_t mradm_dsp_hptf_cascade_clone(const void* handle, void** output, char* message, size_t capacity);
void mradm_dsp_hptf_cascade_destroy(void* handle);
int32_t
mradm_dsp_hptf_cascade_set(void* handle, const MradmDspHptfCoefficients* coefficients, char* message, size_t capacity);
int32_t mradm_dsp_hptf_cascade_reset(void* handle, char* message, size_t capacity);
int32_t mradm_dsp_hptf_cascade_process(void* handle, float* samples, size_t length, char* message, size_t capacity);
int32_t mradm_dsp_hptf_processor_create(size_t channels, uint32_t rate, void** output, char* message, size_t capacity);
void mradm_dsp_hptf_processor_destroy(void* handle);
// A process target is accepted only when not blending. C++ retains/coalesces
// pending publication during a fade; reset always accepts the newest target.
// Failure preserves audio, kernel state and the output update.
int32_t mradm_dsp_hptf_processor_reset(
    void* handle, const MradmDspHptfSnapshot* target, MradmDspHptfUpdate* output, char* message, size_t capacity);
int32_t mradm_dsp_hptf_processor_process(void* handle,
                                         float* samples,
                                         size_t length,
                                         const MradmDspHptfSnapshot* target,
                                         MradmDspHptfUpdate* output,
                                         char* message,
                                         size_t capacity);

// Fixed-rate, interleaved resampling. Lengths count floats; progress counts
// whole frames. end=1 requires empty input and drains to the rational duration.
// A completed stream requires reset before accepting more input.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspResampleProgress {
    size_t input_frames;
    size_t output_frames;
} MradmDspResampleProgress;
// cppcheck-suppress-end unusedStructMember
int32_t mradm_dsp_resampler_create(
    size_t channels, uint32_t input_rate, uint32_t output_rate, void** output, char* message, size_t capacity);
void mradm_dsp_resampler_destroy(void* handle);
int32_t mradm_dsp_resampler_reset(void* handle, char* message, size_t capacity);
int32_t mradm_dsp_resampler_process(void* handle,
                                    const float* input,
                                    size_t input_length,
                                    float* output,
                                    size_t output_length,
                                    uint32_t end,
                                    MradmDspResampleProgress* progress,
                                    char* message,
                                    size_t capacity);

// Binaural spectra contain [L.re, L.im, R.re, R.im] per bin. Live outputs
// are overwritten; OLA outputs are accumulated. All lengths count floats.
int32_t mradm_dsp_live_convolver_create(
    size_t hrtf_length, size_t maximum_frames, uint32_t sample_rate, void** output, char* message, size_t capacity);
void mradm_dsp_live_convolver_destroy(void* handle);
int32_t mradm_dsp_live_state_create(const void* convolver, void** output, char* message, size_t capacity);
void mradm_dsp_live_state_destroy(void* state);
int32_t mradm_dsp_live_state_reset(void* state, char* message, size_t capacity);
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspLiveInfo {
    uint32_t initialized;
    uint32_t tail_remaining;
} MradmDspLiveInfo;
// cppcheck-suppress-end unusedStructMember
int32_t mradm_dsp_live_state_info(const void* state, MradmDspLiveInfo* output, char* message, size_t capacity);
int32_t mradm_dsp_live_initialize(
    void* convolver, void* state, const float* hrtf, size_t hrtf_length, char* message, size_t capacity);
int32_t mradm_dsp_live_process(void* convolver,
                               void* state,
                               const float* hrtf,
                               size_t hrtf_length,
                               const float* input,
                               size_t frames,
                               float* left,
                               size_t left_length,
                               float* right,
                               size_t right_length,
                               uint32_t follows_ramp,
                               char* message,
                               size_t capacity);
int32_t mradm_dsp_ola_create(
    size_t fft_length, size_t overlap, size_t maximum_frames, void** output, char* message, size_t capacity);
void mradm_dsp_ola_destroy(void* handle);
int32_t mradm_dsp_ola_reset(void* handle, char* message, size_t capacity);
// Null end_hrtf with zero length selects a steady filter. Non-null selects a
// block crossfade with the end arm's tail retained, including one-frame blocks.
int32_t mradm_dsp_ola_process(void* handle,
                              const float* input,
                              size_t frames,
                              const float* start_hrtf,
                              size_t start_length,
                              float start_gain,
                              const float* end_hrtf,
                              size_t end_length,
                              float end_gain,
                              float* left,
                              size_t left_length,
                              float* right,
                              size_t right_length,
                              char* message,
                              size_t capacity);
int32_t mradm_dsp_ola_silence(
    void* handle, float* left, size_t left_length, float* right, size_t right_length, char* message, size_t capacity);
int32_t mradm_dsp_diffuse_create(void** output, char* message, size_t capacity);
void mradm_dsp_diffuse_destroy(void* handle);
int32_t mradm_dsp_diffuse_reset(void* handle, char* message, size_t capacity);
int32_t mradm_dsp_diffuse_process(void* handle,
                                  const float* input,
                                  size_t frames,
                                  float* output,
                                  size_t output_length,
                                  char* message,
                                  size_t capacity);
// This entry point alone intentionally edits its sample buffer in place.
int32_t mradm_dsp_diffuse_mix(void* handle,
                              float* samples,
                              size_t frames,
                              float start_gain,
                              float end_gain,
                              float start_diffuse,
                              float end_diffuse,
                              char* message,
                              size_t capacity);

// Private meter modes: 0=I, 1=TP, 2=I+TP, 3=M+S+I. Empty channel map
// preserves the legacy default mapping. Nonempty maps have one position per
// channel, using MeterChannel values from meter.h. Handles are worker-owned.
int32_t mradm_dsp_meter_create(uint32_t channels,
                               uint32_t sample_rate,
                               uint32_t mode,
                               const uint32_t* channel_map,
                               size_t map_length,
                               void** output,
                               char* message,
                               size_t capacity);
void mradm_dsp_meter_destroy(void* handle);
int32_t mradm_dsp_meter_add(void* handle, const float* input, size_t length, char* message, size_t capacity);
int32_t mradm_dsp_meter_reset(void* handle, char* message, size_t capacity);
// Queries: 0=I, 1=M, 2=S, 3=channel TP, 4=max TP. Silence/insufficient
// integrated history is successful -infinity; a silent peak is zero.
int32_t mradm_dsp_meter_query(
    const void* handle, uint32_t query, uint32_t channel, double* output, char* message, size_t capacity);

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
int32_t mradm_dsp_panner_gains(const void* handle,
                               float azimuth,
                               float elevation,
                               float spread,
                               float* output,
                               size_t length,
                               char* message,
                               size_t capacity);
// Immutable HRTF geometry and filters. Shared read-only queries require
// disjoint caller outputs; owners must outlive all queries. Lengths count
// floats except grid entries (one weight and index each).
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspHrtfGridInfo {
    size_t directions;
    size_t entries;
    size_t storage_bytes;
} MradmDspHrtfGridInfo;
typedef struct MradmDspHrtfFilterInfo {
    size_t output_length;
    size_t spectrum_length;
    size_t storage_bytes;
} MradmDspHrtfFilterInfo;
typedef struct MradmDspHrtfTrace {
    float* magnitudes;
    size_t magnitudes_length;
    float* complex_sum;
    size_t complex_sum_length;
    float* scales;
    size_t scales_length;
} MradmDspHrtfTrace;
// cppcheck-suppress-end unusedStructMember
int32_t
mradm_dsp_hrtf_grid_create(const float* directions, size_t length, void** output, char* message, size_t capacity);
void mradm_dsp_hrtf_grid_destroy(void* handle);
int32_t mradm_dsp_hrtf_grid_info(const void* handle, MradmDspHrtfGridInfo* output, char* message, size_t capacity);
int32_t mradm_dsp_hrtf_grid_copy(
    const void* handle, float* weights, int32_t* indices, size_t length, char* message, size_t capacity);
int32_t mradm_dsp_hrtf_grid_index(float azimuth, float elevation, size_t* output, char* message, size_t capacity);
int32_t mradm_dsp_hrtf_filters_create(const void* grid,
                                      const float* impulses,
                                      size_t input_length,
                                      size_t taps,
                                      size_t fft_length,
                                      uint32_t cache_magnitudes,
                                      void** output,
                                      char* message,
                                      size_t capacity);
void mradm_dsp_hrtf_filters_destroy(void* handle);
int32_t mradm_dsp_hrtf_filters_info(const void* handle, MradmDspHrtfFilterInfo* output, char* message, size_t capacity);
int32_t mradm_dsp_hrtf_spectra_copy(const void* handle, float* output, size_t length, char* message, size_t capacity);
// mode=0: quantized offline lookup; mode=1: continuous live lookup.
// Optional trace is accepted only for mode=0. Spectra use re/im floats.
int32_t mradm_dsp_hrtf_query(const void* handle,
                             float azimuth,
                             float elevation,
                             uint32_t mode,
                             float* output,
                             size_t length,
                             const MradmDspHrtfTrace* trace,
                             char* message,
                             size_t capacity);
// Filled by Rust and consumed by other translation units; keep a C-compatible POD.
// cppcheck-suppress-begin unusedStructMember
typedef struct MradmDspDatasetInfo {
    uint32_t sample_rate;
    size_t num_dirs;
    size_t ir_len;
} MradmDspDatasetInfo;
// cppcheck-suppress-end unusedStructMember
int32_t mradm_dsp_dataset_create(
    const uint8_t* bytes, size_t length, int32_t builtin, void** output, char* message, size_t capacity);
void mradm_dsp_dataset_destroy(void* handle);
int32_t mradm_dsp_dataset_info(const void* handle, MradmDspDatasetInfo* info, char* message, size_t capacity);
int32_t mradm_dsp_dataset_copy(const void* handle,
                               float* directions,
                               size_t dir_length,
                               float* impulses,
                               size_t ir_length,
                               char* name,
                               size_t name_capacity,
                               char* message,
                               size_t capacity);
int32_t mradm_dsp_hoa_matrix(float* output, size_t length, char* message, size_t capacity);

int32_t mradm_dsp_spreader_create(const float* ir,
                                  size_t ir_length,
                                  const float* directions,
                                  size_t dir_length,
                                  size_t taps,
                                  uint32_t sample_rate,
                                  const uint64_t* seeds,
                                  size_t sources,
                                  void** output,
                                  char* message,
                                  size_t capacity);
void mradm_dsp_spreader_destroy(void* handle);
int32_t mradm_dsp_spreader_set_source(
    void* handle, size_t index, float azimuth, float elevation, float spread, char* message, size_t capacity);
int32_t mradm_dsp_spreader_process(void* handle,
                                   const float* const* inputs,
                                   size_t sources,
                                   float* left,
                                   float* right,
                                   size_t frames,
                                   char* message,
                                   size_t capacity);
uint32_t mradm_dsp_spreader_delay(void);

#ifdef __cplusplus
}
#endif
