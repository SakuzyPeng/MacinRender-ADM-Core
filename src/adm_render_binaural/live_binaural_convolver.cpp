#include "live_binaural_convolver.h"

#include <algorithm>
#include <bit>
#include <ranges>
#include <saf_utility_fft.h>

namespace mradm::live_scene {

LiveBinauralConvolver::LiveBinauralConvolver(int hrtf_fft_size, std::uint32_t maximum_frames, std::uint32_t sample_rate)
    : hrtf_fft_size_(hrtf_fft_size),
      fft_size_(static_cast<int>(std::bit_ceil(static_cast<std::uint32_t>(hrtf_fft_size) + maximum_frames - 1U))),
      bands_(static_cast<std::size_t>((fft_size_ / 2) + 1)), fade_frames_(std::max(1U, sample_rate / 100U)),
      hrtf_ear_(static_cast<std::size_t>((hrtf_fft_size / 2) + 1)), impulse_(static_cast<std::size_t>(fft_size_), 0.0F),
      input_(static_cast<std::size_t>(fft_size_), 0.0F), source_fd_(bands_), output_fd_(bands_),
      output_(static_cast<std::size_t>(fft_size_)) {
    saf_rfft_create(&hrtf_fft_, hrtf_fft_size_);
    saf_rfft_create(&fft_, fft_size_);
}

LiveBinauralConvolver::~LiveBinauralConvolver() {
    saf_rfft_destroy(&hrtf_fft_);
    saf_rfft_destroy(&fft_);
}

BinauralConvolutionState LiveBinauralConvolver::make_state() const {
    BinauralConvolutionState state;
    state.history.resize(tail_frames(), 0.0F);
    state.current_filter.resize(bands_ * 2U);
    state.target_filter.resize(bands_ * 2U);
    state.target_hrtf.resize(hrtf_ear_.size() * 2U);
    return state;
}

std::uint32_t LiveBinauralConvolver::tail_frames() const noexcept {
    return static_cast<std::uint32_t>(hrtf_fft_size_ - 1);
}

void LiveBinauralConvolver::initialize(BinauralConvolutionState& state, std::span<const float_complex> hrtf) {
    expand_filter(hrtf, state.target_filter);
    std::ranges::copy(hrtf, state.target_hrtf.begin());
    state.current_filter = state.target_filter;
    state.fade_remaining = 0U;
    state.initialized = true;
}

void LiveBinauralConvolver::expand_filter(std::span<const float_complex> hrtf, std::vector<float_complex>& output) {
    // Magnitude/phase interpolation is nonlinear: its inverse FFT can occupy
    // every tap, even when the measured HRIR is much shorter. Retain the entire
    // resulting FIR and zero-pad it for LINEAR convolution; using the HRIR length
    // here silently truncates it and lets the end wrap into each audio block.
    for (std::size_t ear = 0U; ear < 2U; ++ear) {
        for (std::size_t band = 0U; band < hrtf_ear_.size(); ++band) {
            hrtf_ear_[band] = hrtf[(band * 2U) + ear];
        }
        std::ranges::fill(impulse_, 0.0F);
        saf_rfft_backward(hrtf_fft_, hrtf_ear_.data(), impulse_.data());
        saf_rfft_forward(fft_, impulse_.data(), output_fd_.data());
        for (std::size_t band = 0U; band < bands_; ++band) {
            output[(band * 2U) + ear] = output_fd_[band];
        }
    }
}

void LiveBinauralConvolver::filter_ear(std::span<const float_complex> filter, std::size_t ear) {
    for (std::size_t band = 0U; band < bands_; ++band) {
        output_fd_[band] = source_fd_[band] * filter[(band * 2U) + ear];
    }
    saf_rfft_backward(fft_, output_fd_.data(), output_.data());
}

void LiveBinauralConvolver::process(BinauralConvolutionState& state,
                                    std::span<const float_complex> hrtf,
                                    std::span<const float> input,
                                    std::span<float> left,
                                    std::span<float> right,
                                    bool follows_ramp) {
    if (!state.initialized) {
        initialize(state, hrtf);
    } else if (!std::ranges::equal(hrtf, state.target_hrtf)) {
        expand_filter(hrtf, state.target_filter);
        std::ranges::copy(hrtf, state.target_hrtf.begin());
        state.fade_remaining = follows_ramp ? static_cast<std::uint32_t>(input.size()) : fade_frames_;
    }

    const auto history = state.history.size();
    std::ranges::fill(input_, 0.0F);
    std::ranges::copy(state.history, input_.begin());
    std::ranges::copy(input, input_.begin() + static_cast<std::ptrdiff_t>(history));
    saf_rfft_forward(fft_, input_.data(), source_fd_.data());
    for (std::size_t ear = 0U; ear < 2U; ++ear) {
        const auto destination = ear == 0U ? left : right;
        filter_ear(state.current_filter, ear);
        std::copy_n(output_.begin() + static_cast<std::ptrdiff_t>(history), input.size(), destination.begin());
        if (state.fade_remaining != 0U) {
            filter_ear(state.target_filter, ear);
            for (std::size_t index = 0U; index < input.size(); ++index) {
                const float alpha =
                    std::min(1.0F, static_cast<float>(index) / static_cast<float>(state.fade_remaining));
                destination[index] += alpha * (output_[history + index] - destination[index]);
            }
        }
    }
    if (state.fade_remaining != 0U) {
        const auto advanced = std::min(static_cast<std::uint32_t>(input.size()), state.fade_remaining);
        const float alpha = static_cast<float>(advanced) / static_cast<float>(state.fade_remaining);
        for (std::size_t index = 0U; index < state.current_filter.size(); ++index) {
            state.current_filter[index] += alpha * (state.target_filter[index] - state.current_filter[index]);
        }
        state.fade_remaining -= advanced;
        if (state.fade_remaining == 0U) {
            state.current_filter = state.target_filter;
        }
    }
    std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(input.size()), history, state.history.begin());
}

} // namespace mradm::live_scene
