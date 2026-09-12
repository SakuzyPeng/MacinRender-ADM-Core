#pragma once

#include <cstddef>
#include <cstdint>
#include <saf_utility_complex.h>
#include <span>
#include <vector>

namespace mradm::live_scene {

// These module-private workspaces are used by implementation translation units.
// cppcheck-suppress-begin unusedStructMember
// Each source retains INPUT history, so both sides of a filter transition see
// the same samples, including samples preceding the current render segment.
struct BinauralConvolutionState {
    std::vector<float> history;
    std::vector<float_complex> current_filter;
    std::vector<float_complex> target_filter;
    std::vector<float_complex> target_hrtf;
    std::uint32_t fade_remaining{0U};
    std::uint32_t tail_remaining{0U};
    bool initialized{false};
};

class LiveBinauralConvolver {
  public:
    LiveBinauralConvolver(int hrtf_fft_size, std::uint32_t maximum_frames, std::uint32_t sample_rate);
    ~LiveBinauralConvolver();
    LiveBinauralConvolver(const LiveBinauralConvolver&) = delete;
    LiveBinauralConvolver& operator=(const LiveBinauralConvolver&) = delete;
    LiveBinauralConvolver(LiveBinauralConvolver&&) = delete;
    LiveBinauralConvolver& operator=(LiveBinauralConvolver&&) = delete;

    [[nodiscard]] BinauralConvolutionState make_state() const;
    [[nodiscard]] std::uint32_t tail_frames() const noexcept;
    void initialize(BinauralConvolutionState& state, std::span<const float_complex> hrtf);

    // HRTF bins are interleaved L/R. Explicit metadata ramps reach their segment
    // endpoint; discontinuous controls use a persistent 10 ms filter crossfade.
    void process(BinauralConvolutionState& state,
                 std::span<const float_complex> hrtf,
                 std::span<const float> input,
                 std::span<float> left,
                 std::span<float> right,
                 bool follows_ramp);

  private:
    void expand_filter(std::span<const float_complex> hrtf, std::vector<float_complex>& output);
    void filter_ear(std::span<const float_complex> filter, std::size_t ear);

    int hrtf_fft_size_;
    int fft_size_;
    std::size_t bands_;
    std::uint32_t fade_frames_;
    void* hrtf_fft_{nullptr};
    void* fft_{nullptr};
    std::vector<float_complex> hrtf_ear_;
    std::vector<float> impulse_;
    std::vector<float> input_;
    std::vector<float_complex> source_fd_;
    std::vector<float_complex> output_fd_;
    std::vector<float> output_;
};

// cppcheck-suppress-end unusedStructMember
} // namespace mradm::live_scene
