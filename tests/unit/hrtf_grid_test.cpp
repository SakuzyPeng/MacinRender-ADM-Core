#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <saf_utilities.h>
#include <saf_vbap.h>
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
struct Free {
    // Match SAF allocation through an owning unique_ptr.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    void operator()(void* pointer) const noexcept { std::free(pointer); }
};

// Independent reference: SAF triangulation and dense VBAP rows, exactly the
// production path before the optimisation, compressed in bounded batches.
HrtfGrid reference(const std::vector<float>& directions) {
    std::vector<float> triangulation = directions;
    bool bottom = true;
    bool top = true;
    for (std::size_t i = 1; i < directions.size(); i += 2) {
        bottom = bottom && directions[i] > -60.0F;
        top = top && directions[i] < 60.0F;
    }
    if (bottom) {
        triangulation.insert(triangulation.end(), {0.0F, -90.0F});
    }
    if (top) {
        triangulation.insert(triangulation.end(), {0.0F, 90.0F});
    }
    float* vertices = nullptr;
    int* faces = nullptr;
    int vertex_count = 0;
    int face_count = 0;
    std::srand(1);
    findLsTriplets(triangulation.data(),
                   static_cast<int>(triangulation.size() / 2),
                   1,
                   &vertices,
                   &vertex_count,
                   &faces,
                   &face_count);
    const std::unique_ptr<float, Free> vertices_guard(vertices);
    const std::unique_ptr<int, Free> faces_guard(faces);
    require(vertices != nullptr && faces != nullptr && face_count > 0, "SAF triangulation failed");
    std::vector<float> new_vertices;
    std::vector<int> new_faces;
    std::srand(1);
    require(triangulate_hrtf(triangulation, new_vertices, new_faces), "fast hull must handle reference geometry");
    require(new_vertices == std::vector<float>(vertices, vertices + (static_cast<std::ptrdiff_t>(vertex_count) * 3)),
            "triangulation coordinates changed");
    require(new_faces == std::vector<int>(faces, faces + (static_cast<std::ptrdiff_t>(face_count) * 3)),
            "triangulation face order changed");
    float* inverses = nullptr;
    invertLsMtx3D(vertices, faces, face_count, &inverses);
    const std::unique_ptr<float, Free> inverses_guard(inverses);
    HrtfGrid result;
    constexpr std::size_t rows = std::size_t{361} * 181;
    result.gains.assign(rows * 3, 0.0F);
    result.directions.assign(rows * 3, 0);
    for (std::size_t base = 0; base < rows; base += 128) {
        const auto count = std::min(std::size_t{128}, rows - base);
        std::vector<float> queries(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            queries[i * 2] = -180.0F + static_cast<float>((base + i) % 361);
            // Integer quotient is the grid row.
            // NOLINTNEXTLINE(bugprone-integer-division)
            queries[(i * 2) + 1] = -90.0F + static_cast<float>((base + i) / 361);
        }
        float* table = nullptr;
        vbap3D(queries.data(), static_cast<int>(count), vertex_count, faces, face_count, 0.0F, inverses, &table);
        const std::unique_ptr<float, Free> table_guard(table);
        require(table != nullptr, "SAF gain table failed");
        for (std::size_t row = 0; row < count; ++row) {
            std::size_t used = 0;
            float sum = 0.0F;
            for (std::size_t dir = 0; dir < directions.size() / 2; ++dir) {
                const float gain = table[(row * static_cast<std::size_t>(vertex_count)) + dir];
                if (gain > 0.0000001F && used < 3) {
                    result.gains[((base + row) * 3) + used] = gain;
                    result.directions[((base + row) * 3) + used] = static_cast<int>(dir);
                    sum += gain;
                    ++used;
                }
            }
            for (std::size_t i = 0; i < used; ++i) {
                result.gains[((base + row) * 3) + i] /= sum;
            }
        }
    }
    return result;
}
void compare(const std::vector<float>& directions, std::string_view name) {
    const auto expected = reference(directions);
    std::srand(1);
    const auto start = std::chrono::steady_clock::now();
    const auto actual = build_hrtf_grid(directions);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    require(actual != nullptr, "fast gain table failed");
    std::size_t differing = 0;
    for (std::size_t i = 0; i < expected.directions.size(); ++i) {
        differing += static_cast<std::size_t>(actual->directions[i] != expected.directions[i]);
    }
    float maximum = 0;
    for (std::size_t i = 0; i < expected.gains.size(); ++i) {
        require(std::isfinite(actual->gains[i]) && std::isfinite(expected.gains[i]), "nonfinite interpolation weight");
        maximum = std::max(maximum, std::abs(actual->gains[i] - expected.gains[i]));
    }
    std::cout << name << ": fast=" << seconds << " s, differing indices=" << differing
              << ", max weight error=" << maximum << '\n';
    require(differing == 0, "measurement indices changed");
    require(maximum <= 1e-6F, "weight error exceeds float rounding allowance");
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
    require(std::ranges::all_of(first->gains, [](float value) { return std::isfinite(value); }),
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
