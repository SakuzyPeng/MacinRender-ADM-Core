// Release-only timings of the production private FFT API. Planning is untimed.
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp_ffi.h"

namespace {
std::uint64_t bit_hash(const std::vector<float>& samples) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float sample : samples) {
        if (!std::isfinite(sample)) {
            throw std::runtime_error("nonfinite FFT result");
        }
        const auto bits = std::bit_cast<std::uint32_t>(sample);
        for (unsigned byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (8U * byte)) & 255U;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

class FftHandle {
  public:
    explicit FftHandle(std::size_t length) {
        std::array<char, 512> error{};
        if (mradm_dsp_fft_create(length, &handle_, error.data(), error.size()) != 0) {
            throw std::runtime_error(error.data());
        }
    }
    ~FftHandle() { mradm_dsp_fft_destroy(handle_); }
    FftHandle(const FftHandle&) = delete;
    FftHandle& operator=(const FftHandle&) = delete;
    FftHandle(FftHandle&&) = delete;
    FftHandle& operator=(FftHandle&&) = delete;
    [[nodiscard]] void* get() const { return handle_; }

  private:
    void* handle_{};
};

nlohmann::json measure(std::size_t length) {
    FftHandle fft{length};
    std::array<char, 512> error{};
    const auto check = [&](int status) {
        if (status != 0) {
            throw std::runtime_error(error.data());
        }
    };
    std::vector<float> input(length);
    std::vector<float> spectrum(length + 2);
    std::vector<float> output(length);
    std::uint32_t seed = 73;
    for (float& sample : input) {
        seed = (seed * 1664525U) + 1013904223U;
        sample = static_cast<float>(static_cast<std::int32_t>(seed >> 8U) - 8388608) / 8388608.0F;
    }
    const std::size_t iterations = 20000000U / length;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        check(mradm_dsp_fft_forward(
            fft.get(), input.data(), input.size(), spectrum.data(), spectrum.size(), error.data(), error.size()));
    }
    const auto middle = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        check(mradm_dsp_fft_inverse(
            fft.get(), spectrum.data(), spectrum.size(), output.data(), output.size(), error.data(), error.size()));
    }
    const auto end = std::chrono::steady_clock::now();
    using Nanoseconds = std::chrono::duration<double, std::nano>;
    return {{"length", length},
            {"iterations", iterations},
            {"forward_ns", Nanoseconds(middle - start).count() / static_cast<double>(iterations)},
            {"inverse_ns", Nanoseconds(end - middle).count() / static_cast<double>(iterations)},
            {"spectrum_fnv1a64", std::to_string(bit_hash(spectrum))},
            {"inverse_fnv1a64", std::to_string(bit_hash(output))}};
}
} // namespace

int main() try {
    if (MR_ADM_BENCHMARK_RELEASE == 0) {
        std::cerr << "FFT performance evidence requires a Release build\n";
        return 2;
    }
    nlohmann::json measurements = nlohmann::json::array();
    for (const std::size_t length : {128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U, 16384U, 32768U}) {
        measurements.push_back(measure(length));
    }
    std::cout << nlohmann::json{{"schema", "mradm.fft.benchmark.v1"}, {"measurements", measurements}}.dump(2) << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
