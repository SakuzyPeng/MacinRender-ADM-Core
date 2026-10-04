#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binaural_internal.h"

namespace {
using mradm::dsp::Complex;
using namespace mradm::binaural_internal;

void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

// The pre-migration C++ lookup is retained only in this test. It independently
// checks Rust coordinate handling, magnitude/phase blending and bin/ear layout.
class Reference {
  public:
    Reference(const BinauralState& state, mradm::dsp::HrtfGridSnapshot grid, std::vector<Complex> spectra)
        : bins_(static_cast<std::size_t>(state.n_bands)), directions_(static_cast<std::size_t>(state.num_dirs)),
          grid_(std::move(grid)), spectra_(std::move(spectra)) {}

    [[nodiscard]] std::vector<Complex> query(float azimuth, float elevation, bool continuous) const {
        float az = std::fmod(azimuth + 180.0F, 360.0F);
        if (az < 0.0F) {
            az += 360.0F;
        }
        const float el = std::clamp(elevation + 90.0F, 0.0F, 180.0F);
        std::vector<Complex> result(bins_ * 2U);
        if (!continuous) {
            const auto x = std::min(static_cast<std::size_t>(std::lround(az)), std::size_t{360U});
            const auto y = static_cast<std::size_t>(std::lround(el));
            for (std::size_t bin = 0; bin < bins_; ++bin) {
                for (std::size_t ear = 0; ear < 2U; ++ear) {
                    result[(bin * 2U) + ear] = grid_bin((y * 361U) + x, bin, ear);
                }
            }
            return result;
        }
        const auto x0 = static_cast<std::size_t>(std::floor(az));
        const auto y0 = static_cast<std::size_t>(std::floor(el));
        const auto x1 = (x0 + 1U) % 360U;
        const auto y1 = std::min(y0 + 1U, std::size_t{180U});
        const float a = az - static_cast<float>(x0);
        const float e = el - static_cast<float>(y0);
        const std::array<std::size_t, 4> corners{
            (y0 * 361U) + x0, (y0 * 361U) + x1, (y1 * 361U) + x0, (y1 * 361U) + x1};
        const std::array<float, 4> weights{(1.0F - a) * (1.0F - e), a * (1.0F - e), (1.0F - a) * e, a * e};
        for (std::size_t corner = 0; corner < 4U; ++corner) {
            if (weights.at(corner) == 0.0F) {
                continue;
            }
            for (std::size_t bin = 0; bin < bins_; ++bin) {
                for (std::size_t ear = 0; ear < 2U; ++ear) {
                    result[(bin * 2U) + ear] += weights.at(corner) * grid_bin(corners.at(corner), bin, ear);
                }
            }
        }
        return result;
    }

  private:
    [[nodiscard]] Complex grid_bin(std::size_t grid, std::size_t bin, std::size_t ear) const {
        float magnitude = 0.0F;
        Complex sum{0.0F, 0.0F};
        for (std::size_t k = 0; k < 3U; ++k) {
            const float gain = grid_.gains[(grid * 3U) + k];
            const auto direction = static_cast<std::size_t>(grid_.directions[(grid * 3U) + k]);
            const auto h = spectra_[((bin * 2U + ear) * directions_) + direction];
            magnitude += gain * std::abs(h);
            sum += gain * h;
        }
        const float complex_magnitude = std::abs(sum);
        return complex_magnitude > 1e-9F ? sum * (magnitude / complex_magnitude) : Complex{magnitude, 0.0F};
    }
    std::size_t bins_;
    std::size_t directions_;
    mradm::dsp::HrtfGridSnapshot grid_;
    std::vector<Complex> spectra_;
};

void verify(HrtfDataset dataset, std::string_view name) {
    auto offline = build_binaural_state(dataset, 64U);
    auto live = build_binaural_state(std::move(dataset), 64U, true);
    require(offline.has_value() && live.has_value(), "prepare Rust HRTF banks");
    auto spectrum = (*offline)->filters.spectrum_snapshot();
    auto grid = (*offline)->grid->snapshot();
    require(spectrum.has_value() && grid.has_value(), "snapshot Rust HRTF preparation");
    require((*offline)->grid == (*live)->grid, "live and offline banks share cached geometry");
    require((*live)->filters.bytes() - (*offline)->filters.bytes() == spectrum->size() * sizeof(float),
            "only live bank retains magnitude cache");
    const Reference reference(**offline, std::move(*grid), std::move(*spectrum));
    std::vector<std::pair<float, float>> points;
    for (float az : {-540.0F,
                     -180.00001F,
                     -180.0F,
                     -179.99999F,
                     -179.5F,
                     -0.51F,
                     -0.5F,
                     0.0F,
                     0.49F,
                     0.5F,
                     179.49F,
                     179.5F,
                     179.99999F,
                     180.0F,
                     540.0F}) {
        for (float el : {-200.0F, -90.0F, -89.999F, -0.5F, 0.0F, 0.5F, 89.999F, 90.0F, 200.0F}) {
            points.emplace_back(az, el);
        }
    }
    for (std::size_t i = 0; i < 512U; ++i) {
        points.emplace_back((static_cast<float>((i * 7919U) % 72000U) * 0.01F) - 360.0F,
                            (static_cast<float>((i * 3571U) % 24000U) * 0.01F) - 120.0F);
    }
    float maximum_error = 0.0F;
    std::size_t queries = 0U;
    for (const auto& point : points) {
        for (bool continuous : {false, true}) {
            const auto expected = reference.query(point.first, point.second, continuous);
            for (const auto* state : {offline->get(), live->get()}) {
                std::vector<Complex> actual;
                if (continuous) {
                    compute_continuous_hrtf_into(*state, point.first, point.second, actual);
                } else {
                    compute_hrtf_into(*state, point.first, point.second, actual);
                }
                require(actual.size() == expected.size(), "HRTF output shape");
                for (std::size_t index = 0; index < actual.size(); ++index) {
                    const float error = std::abs(actual[index] - expected[index]);
                    maximum_error = std::max(maximum_error, error);
                    require(error <= 2e-6F + (2e-6F * std::abs(expected[index])),
                            "Rust HRTF differs from C++ reference");
                }
                ++queries;
            }
        }
    }
    std::cout << name << ": queries=" << queries << ", max_complex_error=" << maximum_error << '\n';
}

void invalid_preparation() {
    require(!build_binaural_state({}, 64U), "empty dataset rejected");
    auto data = built_in_kemar_dataset();
    require(!build_binaural_state(data, std::numeric_limits<std::uint64_t>::max()), "overflowing block rejected");
    data.hrirs.pop_back();
    require(!build_binaural_state(std::move(data), 64U), "malformed impulse buffer rejected");
}
} // namespace

int main() {
    try {
        verify(built_in_kemar_dataset(), "KEMAR");
        HrtfDataset partial;
        partial.sample_rate = 48000;
        partial.num_dirs = 8;
        partial.hrir_len = 16;
        partial.dirs_deg = {-135, 0, -45, 0, 45, 0, 135, 0, 0, 45, 90, 45, 180, 45, -90, 45};
        partial.hrirs.resize(std::size_t{8U} * 2U * 16U);
        for (std::size_t direction = 0; direction < 8U; ++direction) {
            partial.hrirs[(direction * 32U) + direction] = static_cast<float>(direction + 1U) * 0.125F;
            partial.hrirs[(direction * 32U) + 16U + direction] = -0.25F;
        }
        verify(std::move(partial), "partial sphere");
        invalid_preparation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
