// Optional legacy oracle; this executable is never linked into production.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <saf_hrir.h>
#include <saf_utility_complex.h>
#include <saf_utility_fft.h>
#include <saf_vbap.h>
#include <vector>

#include "dsp.h"
#include "spreader_mr.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void fft_reference() {
    for (const int size : {128, 512, 2048, 8192, 32768}) {
        std::vector<float> input(static_cast<std::size_t>(size));
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<float>((i * 73U) % 251U) / 251.0F - 0.5F;
        }
        std::vector<mradm::dsp::Complex> old(static_cast<std::size_t>(size / 2 + 1)), actual(old.size());
        void* handle = nullptr;
        saf_rfft_create(&handle, size);
        saf_rfft_forward(handle, input.data(), old.data());
        saf_rfft_destroy(&handle);
        mradm::dsp::Fft fft(input.size());
        fft.forward(input.data(), actual.data());
        double error = 0, energy = 0;
        for (std::size_t i = 0; i < old.size(); ++i) {
            error += std::norm(actual[i] - old[i]);
            energy += std::norm(old[i]);
        }
        const double relative = std::sqrt(error / energy);
        std::cout << "FFT " << size << " relative error=" << relative << '\n';
        require(relative < 1e-5, "FFT reference error");
    }
}
void panner_reference() {
    std::vector<float> speakers{0, 0, 90, 0, 180, 0, -90, 0, 0, 90, 0, -90};
    mradm::dsp::Panner panner(speakers, true);
    for (const float spread : {0.0F, 45.0F, 120.0F, 180.0F}) {
        for (const float azimuth : {-160.5F, -65.3F, 17.2F, 117.7F}) {
            std::array<float, 2> source{azimuth, 23.5F};
            float* table = nullptr;
            int rows = 0, triangles = 0;
            generateVBAPgainTable3D_srcs(source.data(), 1, speakers.data(), 6, 1, 1, spread, &table, &rows, &triangles);
            const std::unique_ptr<float, decltype(&std::free)> owner(table, std::free);
            require(table != nullptr && rows == 1, "SAF reference geometry");
            const auto actual = panner.gains(azimuth, source[1], spread);
            for (std::size_t i = 0; i < actual.size(); ++i) {
                require(std::abs(actual[i] - table[i]) < 1e-4F, "VBAP/MDAP reference error");
            }
        }
    }
}
void kemar_reference() {
    void* raw = nullptr;
    std::array<char, 256> error{};
    mradm::dsp::check(mradm_dsp_dataset_create(nullptr, 0, 1, &raw, error.data(), error.size()), error.data());
    const std::unique_ptr<void, decltype(&mradm_dsp_dataset_destroy)> owner(raw, mradm_dsp_dataset_destroy);
    MradmDspDatasetInfo info{};
    mradm::dsp::check(mradm_dsp_dataset_info(raw, &info, error.data(), error.size()), error.data());
    std::vector<float> ir(info.num_dirs * 2U * info.ir_len), directions(info.num_dirs * 2U);
    mradm::dsp::check(
        mradm_dsp_dataset_copy(
            raw, directions.data(), directions.size(), ir.data(), ir.size(), nullptr, 0, error.data(), error.size()),
        error.data());
    require(info.num_dirs == 836U && info.ir_len == 256U && info.sample_rate == 48000U, "KEMAR metadata");
    const float* original = &__default_hrirs[0][0][0];
    for (std::size_t i = 0; i < ir.size(); ++i) {
        require(std::bit_cast<std::uint32_t>(ir[i]) == std::bit_cast<std::uint32_t>(original[i]),
                "KEMAR sample changed");
    }
#ifdef NDEBUG
    void* legacy = nullptr;
    void* modern = nullptr;
    const std::uint64_t seed = 42;
    std::srand(1);
    spreader_create(&legacy);
    spreader_init(legacy, 48000);
    spreader_setNumSources(legacy, 1);
    spreader_setSpreadingMode(legacy, SPREADER_MODE_OM);
    spreader_setAveragingCoeff(legacy, 0.9F);
    spreader_init_from_hrtf_grid(legacy, ir.data(), directions.data(), 836, 2, 256, 48000);
    spreader_initCodec(legacy);
    const auto status = mradm_dsp_spreader_create(ir.data(),
                                                  ir.size(),
                                                  directions.data(),
                                                  directions.size(),
                                                  256,
                                                  48000,
                                                  &seed,
                                                  1,
                                                  &modern,
                                                  error.data(),
                                                  error.size());
    mradm::dsp::check(status, error.data());
    const std::unique_ptr<void, decltype(&mradm_dsp_spreader_destroy)> modern_owner(modern, mradm_dsp_spreader_destroy);
    const auto legacy_delete = [](void* pointer) { spreader_destroy(&pointer); };
    const std::unique_ptr<void, decltype(legacy_delete)> legacy_owner(legacy, legacy_delete);
    spreader_setSourceAzi_deg(legacy, 0, 30);
    spreader_setSourceElev_deg(legacy, 0, 0);
    spreader_setSourceSpread_deg(legacy, 0, 120);
    mradm::dsp::check(mradm_dsp_spreader_set_source(modern, 0, 30, 0, 120, error.data(), error.size()), error.data());
    std::array<float, 512> input{}, old_left{}, old_right{}, left{}, right{};
    const float* in[] = {input.data()};
    float* out[] = {old_left.data(), old_right.data()};
    double old_energy = 0, new_energy = 0;
    std::uint32_t random = 42;
    for (std::size_t block = 0; block < 750; ++block) {
        for (auto& v : input) {
            random = random * 1664525U + 1013904223U;
            v = (static_cast<float>(random >> 8U) / 16777216.0F - 0.5F) * 0.2F;
        }
        spreader_process(legacy, in, out, 1, 2, 512);
        mradm::dsp::check(
            mradm_dsp_spreader_process(modern, in, 1, left.data(), right.data(), 512, error.data(), error.size()),
            error.data());
        if (block > 250) {
            for (std::size_t i = 0; i < 512; ++i) {
                old_energy += old_left[i] * old_left[i] + old_right[i] * old_right[i];
                new_energy += left[i] * left[i] + right[i] * right[i];
            }
        }
    }
    const double delta = 10.0 * std::log10(new_energy / old_energy);
    std::cout << "OM steady noise energy difference=" << delta << " dB\n";
    require(std::isfinite(delta) && std::abs(delta) < 0.5, "OM reference energy changed");
#endif
}
} // namespace
int main() {
    try {
        fft_reference();
        panner_reference();
        kemar_reference();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
