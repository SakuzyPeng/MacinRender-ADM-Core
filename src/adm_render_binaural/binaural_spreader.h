#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace mradm {

// Per-ADM-Object SAF spreader adapter.
// Wraps one spreader instance and provides ring-buffered processing
// compatible with the binaural renderer's variable-size render blocks.
// Output accumulates into caller-provided l_out/r_out (non-interleaved).
//
// Latency: the spreader has a constant STFT processing delay of
// processing_delay() samples (1536 at 48 kHz). prime() pre-fills the output
// ring with one 512-sample frame as a cushion, which makes every process_chunk()
// drain exactly n_frames even for non-frame-aligned render chunks. The adapter's
// input-to-output latency is therefore total_latency(): processing_delay() plus
// the one-frame cushion. The caller (binaural renderer) compensates by delaying
// its OLA path by total_latency(), head-skipping total_latency() output samples,
// and running an equal-length silent tail, so the rendered file is aligned and
// exactly num_frames long.
class BinauralSpreaderAdapter {
  public:
    // hrtf_td:       FLAT [num_dirs × 2 × hrir_len]
    // grid_dirs_deg: FLAT [num_dirs × 2] az/el in degrees (0..360 convention)
    // n_sources:     1 for normal objects, 3 for diverged objects
    BinauralSpreaderAdapter(
        const float* hrtf_td, const float* grid_dirs_deg, int num_dirs, int hrir_len, int sample_rate, int n_sources);
    ~BinauralSpreaderAdapter();
    BinauralSpreaderAdapter(const BinauralSpreaderAdapter&) = delete;
    BinauralSpreaderAdapter& operator=(const BinauralSpreaderAdapter&) = delete;
    BinauralSpreaderAdapter(BinauralSpreaderAdapter&&) noexcept;
    BinauralSpreaderAdapter& operator=(BinauralSpreaderAdapter&&) noexcept;

    // Set direction, spread cone, and gain for source idx.
    // Call once per source before process_chunk() for each render chunk.
    void set_source(int idx, float az_deg, float el_deg, float spread_deg, float gain);

    // Process one render chunk.  mono_ins[0..n_sources_in-1] each point to
    // n_frames samples of per-source mono input (already channel-extracted).
    // Output is *accumulated* into l_out[0..n_frames-1] / r_out[0..n_frames-1].
    void
    process_chunk(const float* const* mono_ins, int n_sources_in, std::size_t n_frames, float* l_out, float* r_out);

    // SAF STFT processing delay in samples (constant: 12 × HOP_SIZE = 1536).
    [[nodiscard]] static int processing_delay();

    // Maximum number of source lanes supported by one SAF spreader instance.
    [[nodiscard]] static int max_sources();

    // Total constant input→output latency after prime(): processing_delay() plus the
    // one-frame ring cushion. This is the amount the caller must compensate for.
    [[nodiscard]] static int total_latency();

    // Pre-fill the output ring with one frame of warm-up so that every subsequent
    // process_chunk() drains exactly n_frames (constant-latency invariant, no gaps).
    // Call once after construction, before the first process_chunk().
    void prime();

  private:
    static constexpr int k_frame_size = 512;
    static constexpr int k_max_sources = 8; // SPREADER_MAX_NUM_SOURCES
    static constexpr int k_n_ears = 2;

    void* handle_{nullptr};
    int n_sources_{0};
    std::array<float, k_max_sources> gains_{};

    // Per-source carry buffers hold < k_frame_size samples between render chunks.
    std::vector<std::vector<float>> carry_; // [n_sources][k_frame_size]
    std::size_t carry_fill_{0};

    // Scratch I/O for spreader_process (non-interleaved, 512 samples each).
    std::vector<std::vector<float>> in_scratch_;       // [n_sources][k_frame_size]
    std::vector<float> out_scratch_l_, out_scratch_r_; // [k_frame_size]
    std::vector<const float*> in_ptrs_;                // pointers into in_scratch_
    std::vector<float*> out_ptrs_;                     // {&out_scratch_l_[0], &out_scratch_r_[0]}

    // Circular output ring: decouples 512-sample STFT batches from variable render
    // chunks. Must hold the STFT processing delay (1536) plus one extra render block
    // worth of output; 32768 gives comfortable margin for any render_block_size ≤ 16384.
    static constexpr std::size_t k_ring_cap = 32768U;
    std::vector<float> out_ring_l_, out_ring_r_; // [k_ring_cap]
    std::size_t out_ring_write_{0};
    std::size_t out_ring_fill_{0};

    void push_batch();
    void drain_ring(float* l_out, float* r_out, std::size_t n_frames);
};

} // namespace mradm
