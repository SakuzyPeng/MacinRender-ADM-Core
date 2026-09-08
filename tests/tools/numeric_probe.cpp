// Release kernel checkpoints. Uses the linked SAF implementation, including its real C RNG.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <span>
#include <vector>

// SAF's complex header must precede headers that open extern "C".
#include <saf.h>
#include <saf_utility_complex.h>

#include "consistency_trace.h"

#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
extern "C" void mr_adm_diagnostic_seed(unsigned int seed);
#endif

namespace {
// Recreating an FFT plan is part of each render's HRTF preparation. Keep both
// same-handle repetitions and new-handle repetitions, with the exact same HRIR.
void probe_fft_lifecycle() {
    constexpr int size = 2048;
    std::vector<float> input(size, 0.0F);
    std::copy_n(&__default_hrirs[0][0][0], 256, input.begin());
    std::vector<std::vector<float_complex>> outputs(16, std::vector<float_complex>((size / 2) + 1));
    std::vector<std::vector<float>> padding;
    for (std::size_t pass = 0; pass < 8U; ++pass) {
        padding.emplace_back(17U + (pass * 331U), 1.0F);
        void* fft = nullptr;
        saf_rfft_create(&fft, size);
        saf_rfft_forward(fft, input.data(), outputs[pass * 2U].data());
        saf_rfft_forward(fft, input.data(), outputs[(pass * 2U) + 1U].data());
        saf_rfft_destroy(&fft);
    }
    mradm::consistency::dump("fft-lifecycle/input.f32", input);
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        mradm::consistency::dump(
            "fft-lifecycle/handle-" + std::to_string(i / 2U) + "-call-" + std::to_string(i % 2U) + ".c32", outputs[i]);
    }
}
} // namespace

int main() {
    using mradm::consistency::dump;
    if (std::getenv("MR_ADM_TRACE_DIR") == nullptr) {
        std::cerr << "MR_ADM_TRACE_DIR is required\n";
        return 2;
    }
    probe_fft_lifecycle();
    // A runtime operand prevents the compiler from replacing the libm calls with constants.
    volatile float angle = 30.0F;
    const float az = angle * (static_cast<float>(std::numbers::pi) / 180.0F);
    const float x = std::cos(az);
    const float y = std::sin(az);
    dump("math.01-direction.f32", {az, x, y});
    dump("math.02-hypot.f32", {std::hypot(x, y, 0.0F), std::sqrt((x * x) + (y * y))});

    constexpr int n = 2048;
    std::vector<float> input(n);
    std::uint32_t state = 0x12345678U;
    for (float& sample : input) {
        state = (state * 1664525U) + 1013904223U;
        sample = static_cast<float>(static_cast<std::int32_t>(state >> 8U) - 8388608) / 8388608.0F;
    }
    std::vector<float_complex> fd((n / 2) + 1);
    std::vector<float> output(n);
    void* fft = nullptr;
    saf_rfft_create(&fft, n);
    saf_rfft_forward(fft, input.data(), fd.data());
    saf_rfft_backward(fft, fd.data(), output.data());
    saf_rfft_destroy(&fft);
    dump("fft.01-input.f32", input);
    dump("fft.02-forward.c32", fd);
    dump("fft.03-backward.f32", output);

    constexpr int bands = 133;
    constexpr int channels = 2;
    std::vector<float> frequencies(bands);
    void* stft = nullptr;
    afSTFT_create(&stft, 1, channels, 128, 0, 1, AFSTFT_BANDS_CH_TIME);
    afSTFT_getCentreFreqs(stft, 48000.0F, bands, frequencies.data());
    afSTFT_destroy(&stft);
    dump("decor.01-frequencies.f32", frequencies);
#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
    dump("decor.02-rand-max.i32", {2147483647});
    mr_adm_diagnostic_seed(1);
#else
    dump("decor.02-rand-max.i32", {RAND_MAX});
#endif
    std::srand(1);
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 2) {
            std::srand(1);
#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
            mr_adm_diagnostic_seed(1);
#endif
        }
        // spreader_initCodec constructs all eight lanes, including unused lanes.
        std::vector<int> all_delays;
        for (int lane = 0; lane < 8; ++lane) {
            std::array<int, static_cast<std::size_t>(bands) * channels> delays{};
            getDecorrelationDelays(channels, frequencies.data(), bands, 48000.0F, 12, 128, delays.data());
            all_delays.insert(all_delays.end(), delays.begin(), delays.end());
        }
        dump("decor.03-delays-pass-" + std::to_string(pass) + ".i32", all_delays);
    }
    return 0;
}
