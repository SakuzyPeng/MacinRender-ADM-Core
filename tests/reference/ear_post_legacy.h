// Frozen test-only EAR post-processing arithmetic from e12332b.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp.h"
#include "render_common.h"
namespace ear_post_legacy {
namespace dsp = mradm::dsp;
[[nodiscard]] std::size_t next_power_of_two(std::size_t value) {
    std::size_t out = 1;
    while (out < value) {
        out <<= 1U;
    }
    return out;
}

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct DecorrState {
    dsp::FftHandle hFFT{nullptr};                     // Rust FFT plan, L=2048
    std::vector<std::vector<dsp::Complex>> filter_fd; // [num_out_ch][L/2+1=1025]
    std::vector<std::vector<float>> overlap;          // [num_out_ch][K-1=511]
    std::size_t fft_len{0};
    std::size_t bins{0};
    std::size_t overlap_len{0};
    int comp_delay{0};                         // decorrelatorCompensationDelay()=255
    std::vector<std::vector<float>> dir_delay; // [num_out_ch][comp_delay]

    ~DecorrState() {
        if (hFFT != nullptr) {
            dsp::fft_destroy(&hFFT);
        }
    }
    DecorrState() = default;
    DecorrState(const DecorrState&) = delete;
    DecorrState& operator=(const DecorrState&) = delete;
    DecorrState(DecorrState&&) = delete;
    DecorrState& operator=(DecorrState&&) = delete;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)
void apply_decorrelator(DecorrState& state,
                        const std::vector<float>& diffuse_in,
                        std::vector<float>& diffuse_out,
                        std::size_t frames_now,
                        std::size_t num_out_ch) {
    // Per-call scratch — small fixed size, stack-friendly via vector.
    std::vector<float> buf(state.fft_len);
    std::vector<dsp::Complex> x_fd(state.bins);
    std::vector<dsp::Complex> y_fd(state.bins);
    std::vector<float> y(state.fft_len);

    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        // Deinterleave, zero-pad remainder.
        std::ranges::fill(buf, 0.0F);
        for (std::size_t f = 0; f < frames_now; ++f) {
            buf[f] = diffuse_in[(f * num_out_ch) + ch];
        }

        dsp::fft_forward(state.hFFT, buf.data(), x_fd.data());

        for (std::size_t b = 0; b < state.bins; ++b) {
            y_fd[b] = x_fd[b] * state.filter_fd[ch][b];
        }

        // dsp::fft_inverse scales by 1/N internally — no extra scaling needed.
        dsp::fft_inverse(state.hFFT, y_fd.data(), y.data());

        // Overlap-add: accumulate saved tail into this block's output.
        auto& ovl = state.overlap[ch];
        for (std::size_t f = 0; f < frames_now; ++f) {
            diffuse_out[(f * num_out_ch) + ch] = y[f] + (f < state.overlap_len ? ovl[f] : 0.0F);
        }

        // Save new tail (y[frames_now .. frames_now + k_overlap_len - 1]).
        for (std::size_t i = 0; i < state.overlap_len; ++i) {
            ovl[i] = y[frames_now + i];
        }
    }
}

// Delay the direct bus by comp_delay samples using a circular history buffer.
// Operates in-place on direct_block [frames_now × num_out_ch].
void apply_direct_delay(DecorrState& state,
                        std::vector<float>& direct_block,
                        std::size_t frames_now,
                        std::size_t num_out_ch) {
    const auto delay = static_cast<std::size_t>(state.comp_delay); // 255

    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        auto& buf = state.dir_delay[ch]; // [delay]

        // Snapshot the new samples before in-place modification.
        std::vector<float> new_in(frames_now);
        for (std::size_t f = 0; f < frames_now; ++f) {
            new_in[f] = direct_block[(f * num_out_ch) + ch];
        }

        // Output: up to D samples from the delay buffer, then new_in offset by D.
        // Works for any frames_now, including short tail blocks < D.
        const std::size_t from_buf = std::min(delay, frames_now);
        for (std::size_t f = 0; f < from_buf; ++f) {
            direct_block[(f * num_out_ch) + ch] = buf[f];
        }
        for (std::size_t f = from_buf; f < frames_now; ++f) {
            direct_block[(f * num_out_ch) + ch] = new_in[f - delay];
        }

        // Update delay buffer: evict consumed samples, append new_in.
        if (frames_now >= delay) {
            std::ranges::copy(new_in.end() - static_cast<std::ptrdiff_t>(delay), new_in.end(), buf.begin());
        } else {
            // Shift remaining delay left by frames_now, then append new_in at end.
            std::ranges::copy(buf.begin() + static_cast<std::ptrdiff_t>(frames_now), buf.end(), buf.begin());
            std::ranges::copy(new_in, buf.end() - static_cast<std::ptrdiff_t>(frames_now));
        }
    }
}

void init_decorr_state(DecorrState& decorr,
                       const std::vector<std::vector<float>>& raw_filters,
                       uint16_t num_out_ch,
                       uint64_t k_block_size) {
    constexpr std::size_t k_fir_len = 512;
    const std::size_t k_fft_len = next_power_of_two(static_cast<std::size_t>(k_block_size) + k_fir_len - 1U);
    const std::size_t k_bins = (k_fft_len / 2U) + 1U;

    dsp::fft_create(&decorr.hFFT, static_cast<int>(k_fft_len));
    decorr.fft_len = k_fft_len;
    decorr.bins = k_bins;
    decorr.overlap_len = k_fir_len - 1U;
    decorr.comp_delay = 255; // 255
    decorr.overlap.assign(num_out_ch, std::vector<float>(decorr.overlap_len, 0.0F));
    decorr.dir_delay.assign(num_out_ch, std::vector<float>(static_cast<std::size_t>(decorr.comp_delay), 0.0F));

    decorr.filter_fd.resize(num_out_ch, std::vector<dsp::Complex>(k_bins));
    std::vector<float> fir_buf(k_fft_len, 0.0F);
    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        std::ranges::fill(fir_buf, 0.0F);
        const auto& fir = raw_filters[ch];
        std::ranges::copy(fir, fir_buf.begin());
        dsp::fft_forward(decorr.hFFT, fir_buf.data(), decorr.filter_fd[ch].data());
    }
}

} // namespace ear_post_legacy
