#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "binaural_internal.h"

namespace {
using namespace mradm::binaural_internal;
void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}
void compare(const std::vector<float>& directions, std::string_view name) {
    const auto start = std::chrono::steady_clock::now();
    const auto owner = HrtfGrid::create(directions);
    require(owner.has_value(), "Rust HRTF grid preparation failed");
    const auto actual = owner->snapshot();
    require(actual.has_value(), "grid snapshot failed");
    std::srand(7123);
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.rand): verify independence from legacy process RNG.
    (void) std::rand();
    const auto repeated_owner = HrtfGrid::create(directions);
    require(repeated_owner.has_value(), "repeated grid preparation failed");
    const auto repeat = repeated_owner->snapshot();
    require(repeat.has_value() && repeat->directions == actual->directions && repeat->gains == actual->gains,
            "grid depends on process RNG history");
    require(actual->gains.size() == std::size_t{361} * 181U * 3U && actual->directions.size() == actual->gains.size(),
            "grid dimensions changed");
    for (std::size_t row = 0; row < actual->gains.size() / 3U; ++row) {
        float sum = 0.0F;
        for (std::size_t lane = 0; lane < 3U; ++lane) {
            const auto index = (row * 3U) + lane;
            require(std::isfinite(actual->gains[index]) && actual->gains[index] >= 0.0F,
                    "invalid interpolation weight");
            require(actual->directions[index] >= 0 &&
                        static_cast<std::size_t>(actual->directions[index]) < directions.size() / 2U,
                    "interpolation index out of bounds");
            sum += actual->gains[index];
        }
        require(sum == 0.0F || std::abs(sum - 1.0F) < 1e-6F, "amplitude weights are not normalized");
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << name << ": two Rust grid builds " << seconds << " s\n";
}
} // namespace
int main() {
    try {
        compare(built_in_kemar_dataset().dirs_deg, "KEMAR");
        const std::vector<float> partial{-135, 0, -45, 0, 45, 0, 135, 0, 0, 45, 90, 45, 180, 45, -90, 45};
        compare(partial, "partial sphere with dummy poles");
        // The geometry cache is Rust mradm-render `binaural_hrtf` (PCM consistency 4.7 A) and tested there.
        auto nonfinite = partial;
        nonfinite[0] = std::numeric_limits<float>::quiet_NaN();
        require(!HrtfGrid::create(nonfinite), "nonfinite directions must be rejected");
        if (const char* file = std::getenv("MR_ADM_TEST_SOFA_PATH")) {
            auto dataset = load_sofa_dataset(file, 0);
            require(dataset.has_value(), "SOFA fixture cannot be loaded");
            compare(dataset->dirs_deg, "SOFA fixture");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
