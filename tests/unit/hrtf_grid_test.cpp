#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "binaural_internal.h"
#include "hrtf_grid.h"

namespace {
using namespace mradm::binaural_internal;
void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}
void compare(const std::vector<float>& directions, std::string_view name) {
    const auto start = std::chrono::steady_clock::now();
    const auto owner = build_hrtf_grid(directions);
    require(owner != nullptr, "Rust HRTF grid preparation failed");
    const auto actual = owner->snapshot();
    require(actual.has_value(), "grid snapshot failed");
    std::srand(7123);
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.rand): verify independence from legacy process RNG.
    (void) std::rand();
    const auto repeated_owner = build_hrtf_grid(directions);
    require(repeated_owner != nullptr, "repeated grid preparation failed");
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
void caching(std::vector<float> directions) {
    std::array<std::future<std::shared_ptr<const HrtfGrid>>, 4> tasks;
    for (auto& task : tasks) {
        task = std::async(std::launch::async, [&] { return prepare_hrtf_grid(directions); });
    }
    auto first = tasks.at(0).get();
    require(first != nullptr, "cached table missing");
    for (std::size_t i = 1; i < tasks.size(); ++i) {
        require(tasks.at(i).get() == first, "concurrent cache miss was not coalesced");
    }
    std::swap(directions[0], directions[2]);
    std::swap(directions[1], directions[3]);
    require(prepare_hrtf_grid(directions) != first, "direction ordering is part of cache identity");
    for (int i = 0; i < 10; ++i) {
        directions[0] += 0.013F;
        require(prepare_hrtf_grid(directions) != nullptr, "cache eviction build failed");
    }
    require(hrtf_grid_cache_bytes() <= std::size_t{16} * 1024U * 1024U, "geometry cache exceeded byte budget");
    const auto retained = first->snapshot();
    require(retained.has_value(), "retained grid snapshot failed");
    require(std::ranges::all_of(retained->gains, [](float value) { return std::isfinite(value); }),
            "eviction damaged retained table");
    directions[0] = std::numeric_limits<float>::quiet_NaN();
    require(!prepare_hrtf_grid(directions), "nonfinite directions must be rejected");
}
} // namespace
int main() {
    try {
        compare(built_in_kemar_dataset().dirs_deg, "KEMAR");
        const std::vector<float> partial{-135, 0, -45, 0, 45, 0, 135, 0, 0, 45, 90, 45, 180, 45, -90, 45};
        compare(partial, "partial sphere with dummy poles");
        caching(partial);
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
