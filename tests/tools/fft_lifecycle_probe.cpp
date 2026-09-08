// Release-only SAF FFT lifecycle probe. Supply a canonical HRIR checkpoint (.f32).
// Repeated calls on one handle and calls on newly created handles are reported separately.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <saf.h>
#include <saf_utility_complex.h>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: mr_adm_fft_lifecycle_probe <canonical-hrir.f32>\n";
        return 2;
    }
    constexpr int size = 2048;
    std::vector<float> input(size, 0.0F);
    std::ifstream file(argv[1], std::ios::binary);
    for (std::size_t i = 0; i < 256U; ++i) {
        std::uint32_t bits = 0;
        for (unsigned byte = 0; byte < 4U; ++byte) {
            const int value = file.get();
            if (value == std::char_traits<char>::eof()) {
                std::cerr << "missing or truncated HRIR input\n";
                return 2;
            }
            bits |= static_cast<std::uint32_t>(value) << (byte * 8U);
        }
        input[i] = std::bit_cast<float>(bits);
    }
    constexpr int bands = (size / 2) + 1;
    std::vector<float_complex> reference(bands);
    std::vector<float_complex> out(bands);
    std::vector<float_complex> within(bands);
    const auto same_bits = [](const float_complex& first, const float_complex& second) {
        return std::bit_cast<std::uint32_t>(first.real()) == std::bit_cast<std::uint32_t>(second.real()) &&
               std::bit_cast<std::uint32_t>(first.imag()) == std::bit_cast<std::uint32_t>(second.imag());
    };
    for (std::size_t pass = 0; pass < 32U; ++pass) {
        // Vary the heap history between plan creations. This is a diagnostic perturbation;
        // none of these samples are passed to the transform.
        const std::vector<float> padding(17U + (pass * 331U), 1.0F);
        void* fft = nullptr;
        saf_rfft_create(&fft, size);
        saf_rfft_forward(fft, input.data(), out.data());
        saf_rfft_forward(fft, input.data(), within.data());
        if (pass == 0U) {
            reference = out;
        }
        std::size_t different = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!same_bits(out[i], reference[i])) {
                ++different;
            }
        }
        std::cout << "pass=" << pass << " padding_samples=" << padding.size() << " dc_bits=" << std::hex
                  << std::bit_cast<std::uint32_t>(out[0].real()) << std::dec << " changed_bins=" << different
                  << " same_handle_equal=" << std::equal(out.begin(), out.end(), within.begin(), same_bits) << "\n";
        saf_rfft_destroy(&fft);
    }
    return 0;
}
