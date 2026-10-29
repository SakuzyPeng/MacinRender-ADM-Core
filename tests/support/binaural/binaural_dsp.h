#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "dsp.h"

namespace mradm::dsp {

namespace detail {
template <auto Destroy> struct BinauralDeleter {
    void operator()(void* handle) const noexcept { Destroy(handle); }
};
template <auto Destroy> using BinauralHandle = std::unique_ptr<void, BinauralDeleter<Destroy>>;

inline const float* spectrum_data(std::span<const Complex> spectrum) noexcept {
    // C++ guarantees array-oriented float access to std::complex<float>.
    return reinterpret_cast<const float*>(spectrum.data());
}
} // namespace detail

// Private renderer helpers use the existing dsp::check exception boundary.
// Owners are movable, never copyable. Calls on an owner must be serialized;
// different owners (including all OLA workspaces) may run on different workers.
class LiveConvolutionState final {
  public:
    LiveConvolutionState() = default;
    [[nodiscard]] MradmDspLiveInfo info() const {
        MradmDspLiveInfo result{};
        std::array<char, 256> error{};
        check(mradm_dsp_live_state_info(handle_.get(), &result, error.data(), error.size()), error.data());
        return result;
    }
    void reset() {
        std::array<char, 256> error{};
        check(mradm_dsp_live_state_reset(handle_.get(), error.data(), error.size()), error.data());
    }

  private:
    friend class LiveConvolver;
    detail::BinauralHandle<mradm_dsp_live_state_destroy> handle_;
};

class LiveConvolver final {
  public:
    LiveConvolver(std::size_t hrtf_length, std::uint32_t maximum_frames, std::uint32_t sample_rate) {
        std::array<char, 256> error{};
        void* handle = nullptr;
        check(mradm_dsp_live_convolver_create(
                  hrtf_length, maximum_frames, sample_rate, &handle, error.data(), error.size()),
              error.data());
        handle_.reset(handle);
        tail_frames_ = static_cast<std::uint32_t>(hrtf_length - 1U);
    }
    [[nodiscard]] LiveConvolutionState make_state() const {
        std::array<char, 256> error{};
        void* handle = nullptr;
        check(mradm_dsp_live_state_create(handle_.get(), &handle, error.data(), error.size()), error.data());
        LiveConvolutionState state;
        state.handle_.reset(handle);
        return state;
    }
    [[nodiscard]] std::uint32_t tail_frames() const noexcept { return tail_frames_; }
    void initialize(LiveConvolutionState& state, std::span<const Complex> hrtf) {
        std::array<char, 256> error{};
        check(mradm_dsp_live_initialize(handle_.get(),
                                        state.handle_.get(),
                                        detail::spectrum_data(hrtf),
                                        hrtf.size() * 2U,
                                        error.data(),
                                        error.size()),
              error.data());
    }
    void process(LiveConvolutionState& state,
                 std::span<const Complex> hrtf,
                 std::span<const float> input,
                 std::span<float> left,
                 std::span<float> right,
                 bool follows_ramp) {
        std::array<char, 256> error{};
        check(mradm_dsp_live_process(handle_.get(),
                                     state.handle_.get(),
                                     detail::spectrum_data(hrtf),
                                     hrtf.size() * 2U,
                                     input.data(),
                                     input.size(),
                                     left.data(),
                                     left.size(),
                                     right.data(),
                                     right.size(),
                                     follows_ramp ? 1U : 0U,
                                     error.data(),
                                     error.size()),
              error.data());
    }

  private:
    detail::BinauralHandle<mradm_dsp_live_convolver_destroy> handle_;
    std::uint32_t tail_frames_{};
};

class OlaConvolver final {
  public:
    OlaConvolver() = default;
    OlaConvolver(std::size_t fft_length, std::size_t overlap, std::size_t maximum_frames) {
        std::array<char, 256> error{};
        void* handle = nullptr;
        check(mradm_dsp_ola_create(fft_length, overlap, maximum_frames, &handle, error.data(), error.size()),
              error.data());
        handle_.reset(handle);
    }
    void reset() {
        std::array<char, 256> error{};
        check(mradm_dsp_ola_reset(handle_.get(), error.data(), error.size()), error.data());
    }
    void process(std::span<const float> input,
                 std::span<const Complex> hrtf,
                 float gain,
                 std::span<float> left,
                 std::span<float> right) {
        process_filters(input, hrtf, gain, {}, gain, left, right);
    }
    void crossfade(std::span<const float> input,
                   std::span<const Complex> start_hrtf,
                   float start_gain,
                   std::span<const Complex> end_hrtf,
                   float end_gain,
                   std::span<float> left,
                   std::span<float> right) {
        process_filters(input, start_hrtf, start_gain, end_hrtf, end_gain, left, right);
    }
    void advance_silence(std::span<float> left, std::span<float> right) {
        std::array<char, 256> error{};
        check(mradm_dsp_ola_silence(
                  handle_.get(), left.data(), left.size(), right.data(), right.size(), error.data(), error.size()),
              error.data());
    }

  private:
    void process_filters(std::span<const float> input,
                         std::span<const Complex> start_hrtf,
                         float start_gain,
                         std::span<const Complex> end_hrtf,
                         float end_gain,
                         std::span<float> left,
                         std::span<float> right) {
        std::array<char, 256> error{};
        check(mradm_dsp_ola_process(handle_.get(),
                                    input.data(),
                                    input.size(),
                                    detail::spectrum_data(start_hrtf),
                                    start_hrtf.size() * 2U,
                                    start_gain,
                                    detail::spectrum_data(end_hrtf),
                                    end_hrtf.size() * 2U,
                                    end_gain,
                                    left.data(),
                                    left.size(),
                                    right.data(),
                                    right.size(),
                                    error.data(),
                                    error.size()),
              error.data());
    }
    detail::BinauralHandle<mradm_dsp_ola_destroy> handle_;
};

class DiffuseDelay final {
  public:
    DiffuseDelay() {
        std::array<char, 256> error{};
        void* handle = nullptr;
        check(mradm_dsp_diffuse_create(&handle, error.data(), error.size()), error.data());
        handle_.reset(handle);
    }
    void reset() {
        std::array<char, 256> error{};
        check(mradm_dsp_diffuse_reset(handle_.get(), error.data(), error.size()), error.data());
    }
    void process(std::span<const float> input, std::span<float> output) {
        std::array<char, 256> error{};
        check(mradm_dsp_diffuse_process(
                  handle_.get(), input.data(), input.size(), output.data(), output.size(), error.data(), error.size()),
              error.data());
    }
    void mix(std::span<float> samples, float start_gain, float end_gain, float start_diffuse, float end_diffuse) {
        std::array<char, 256> error{};
        check(mradm_dsp_diffuse_mix(handle_.get(),
                                    samples.data(),
                                    samples.size(),
                                    start_gain,
                                    end_gain,
                                    start_diffuse,
                                    end_diffuse,
                                    error.data(),
                                    error.size()),
              error.data());
    }

  private:
    detail::BinauralHandle<mradm_dsp_diffuse_destroy> handle_;
};

} // namespace mradm::dsp
