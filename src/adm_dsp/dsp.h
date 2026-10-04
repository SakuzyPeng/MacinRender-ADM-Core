#pragma once

#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dsp_ffi.h"

namespace mradm::dsp {

using Complex = std::complex<float>;

inline void check(int status, const char* message) {
    if (status != 0) {
        throw std::runtime_error(std::string{"Rust DSP: "} + message);
    }
}

// FFT owns its Rust plan and scratch; processing neither allocates nor mutates
// the caller's input. C++ guarantees the array-oriented float access used below
// for std::complex<float> ([complex.syn]); Rust sees only interleaved floats.
class Fft final {
  public:
    explicit Fft(std::size_t length) : length_(length), bins_((length / 2U) + 1U) {
        std::array<char, 256> error{};
        check(mradm_dsp_fft_create(length_, &handle_, error.data(), error.size()), error.data());
    }
    ~Fft() { mradm_dsp_fft_destroy(handle_); }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;
    Fft(Fft&&) = delete;
    Fft& operator=(Fft&&) = delete;

    void forward(const float* input, Complex* output) {
        std::array<char, 256> error{};
        check(mradm_dsp_fft_forward(
                  handle_, input, length_, reinterpret_cast<float*>(output), bins_ * 2U, error.data(), error.size()),
              error.data());
    }
    void inverse(const Complex* input, float* output) {
        std::array<char, 256> error{};
        check(mradm_dsp_fft_inverse(handle_,
                                    reinterpret_cast<const float*>(input),
                                    bins_ * 2U,
                                    output,
                                    length_,
                                    error.data(),
                                    error.size()),
              error.data());
    }

  private:
    void* handle_{nullptr};
    std::size_t length_;
    std::size_t bins_;
};

using FftHandle = std::unique_ptr<Fft>;

inline void fft_create(FftHandle* handle, int length) {
    *handle = std::make_unique<Fft>(static_cast<std::size_t>(length));
}
inline void fft_destroy(FftHandle* handle) {
    handle->reset();
}
inline void fft_forward(const FftHandle& handle, const float* input, Complex* output) {
    handle->forward(input, output);
}
inline void fft_inverse(const FftHandle& handle, const Complex* input, float* output) {
    handle->inverse(input, output);
}

class Panner final {
  public:
    Panner(std::span<const float> directions, bool is_3d) : channels_(directions.size() / 2U) {
        std::array<char, 256> error{};
        check(mradm_dsp_panner_create(
                  directions.data(), directions.size(), is_3d ? 1 : 0, &handle_, error.data(), error.size()),
              error.data());
    }
    ~Panner() { mradm_dsp_panner_destroy(handle_); }
    Panner(const Panner&) = delete;
    Panner& operator=(const Panner&) = delete;
    Panner(Panner&&) = delete;
    Panner& operator=(Panner&&) = delete;
    [[nodiscard]] std::vector<float> gains(float azimuth, float elevation, float spread) const {
        std::vector<float> result(channels_);
        std::array<char, 256> error{};
        check(mradm_dsp_panner_gains(
                  handle_, azimuth, elevation, spread, result.data(), result.size(), error.data(), error.size()),
              error.data());
        return result;
    }

  private:
    void* handle_{nullptr};
    std::size_t channels_;
};

// Offline block preparation and live metadata updates reuse immutable geometry.
// The bounded cache does not depend on the scene or own any audio buffers.
inline std::shared_ptr<const Panner> panner_for(std::span<const float> directions, bool is_3d) {
    struct Entry {
        std::vector<float> directions;
        bool is_3d{false};
        std::shared_ptr<const Panner> panner;
    };
    static std::mutex mutex;
    static std::vector<Entry> entries;
    const std::lock_guard lock(mutex);
    const auto found = std::ranges::find_if(entries, [&](const Entry& entry) {
        return entry.is_3d == is_3d && std::ranges::equal(entry.directions, directions);
    });
    if (found != entries.end()) {
        return found->panner;
    }
    auto panner = std::make_shared<const Panner>(directions, is_3d);
    if (entries.size() == 32U) {
        entries.erase(entries.begin());
    }
    entries.push_back({std::vector<float>(directions.begin(), directions.end()), is_3d, panner});
    return panner;
}

} // namespace mradm::dsp
