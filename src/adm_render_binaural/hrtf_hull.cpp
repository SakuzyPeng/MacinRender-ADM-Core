/*
 Copyright (c) 2017-2018 Leo McCormack

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/


// Adapted from SAF/convhull_3d (Leo McCormack), itself derived from George
// Papazafeiropoulos' computational-geometry-toolbox (BSD-2-Clause, 2014).
// Keep the original point perturbation, insertion order and arithmetic. Only
// replace horizon membership scans and temporary allocations with edge adjacency.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex> // Load C++ headers before SAF's extern-C BLAS/Accelerate wrapper.
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numeric>
#include <saf_externals.h>
#include <saf_utilities.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hrtf_grid.h"

namespace mradm::binaural_internal {
namespace {
using Point = std::array<double, 4>;
using Triangle = std::array<int, 3>;
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();
constexpr std::size_t max_faces = 50000U;
struct Face {
    Triangle vertices{};
    std::array<double, 3> normal{};
    double offset{0.0};
    bool visible{false};
};

double determinant(const std::array<double, 16>& m) {
    return (m[3] * m[6] * m[9] * m[12]) - (m[2] * m[7] * m[9] * m[12]) - (m[3] * m[5] * m[10] * m[12]) +
           (m[1] * m[7] * m[10] * m[12]) + (m[2] * m[5] * m[11] * m[12]) - (m[1] * m[6] * m[11] * m[12]) -
           (m[3] * m[6] * m[8] * m[13]) + (m[2] * m[7] * m[8] * m[13]) + (m[3] * m[4] * m[10] * m[13]) -
           (m[0] * m[7] * m[10] * m[13]) - (m[2] * m[4] * m[11] * m[13]) + (m[0] * m[6] * m[11] * m[13]) +
           (m[3] * m[5] * m[8] * m[14]) - (m[1] * m[7] * m[8] * m[14]) - (m[3] * m[4] * m[9] * m[14]) +
           (m[0] * m[7] * m[9] * m[14]) + (m[1] * m[4] * m[11] * m[14]) - (m[0] * m[5] * m[11] * m[14]) -
           (m[2] * m[5] * m[8] * m[15]) + (m[1] * m[6] * m[8] * m[15]) + (m[2] * m[4] * m[9] * m[15]) -
           (m[0] * m[6] * m[9] * m[15]) - (m[1] * m[4] * m[10] * m[15]) + (m[0] * m[5] * m[10] * m[15]);
}


Face make_face(Triangle vertices, const std::vector<Point>& points) {
    Face face;
    face.vertices = vertices;
    std::array<std::array<double, 3>, 2> differences{};
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            differences.at(i).at(j) = points[static_cast<std::size_t>(vertices.at(i + 1))].at(j) -
                                      points[static_cast<std::size_t>(vertices.at(i))].at(j);
        }
    }
    double sign = 1.0;
    for (std::size_t i = 0; i < 3; ++i) {
        std::array<std::array<double, 2>, 2> sub{};
        for (std::size_t j = 0; j < 2; ++j) {
            std::size_t column = 0;
            for (std::size_t k = 0; k < 3; ++k) {
                if (k != i) {
                    sub.at(j).at(column++) = differences.at(j).at(k);
                }
            }
        }
        face.normal.at(i) = sign * (sub.at(0).at(0) * sub.at(1).at(1) - sub.at(1).at(0) * sub.at(0).at(1));
        sign *= -1.0;
    }
    double norm = std::accumulate(face.normal.begin(), face.normal.end(), 0.0, [](double sum, double value) {
        return sum + std::pow(value, 2.0);
    });
    norm = std::sqrt(norm);
    std::ranges::transform(face.normal, face.normal.begin(), [norm](double value) { return value / norm; });
    for (std::size_t i = 0; i < 3; ++i) {
        face.offset += -points[static_cast<std::size_t>(vertices.at(0))].at(i) * face.normal.at(i);
    }
    return face;
}

bool orient(Face& face, const std::vector<Point>& points, std::size_t start) {
    std::array<double, 16> matrix{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            matrix.at((row * 4) + col) = points[static_cast<std::size_t>(face.vertices.at(row))].at(col);
        }
    }
    for (std::size_t point = start; point < points.size(); ++point) {
        if (std::ranges::find(face.vertices, static_cast<int>(point)) != face.vertices.end()) {
            continue;
        }
        std::copy(points[point].begin(), points[point].end(), matrix.begin() + 12);
        const double value = determinant(matrix);
        if (!std::isfinite(value)) {
            return false;
        }
        if (value == 0.0) {
            continue;
        }
        if (value < 0.0) {
            std::swap(face.vertices.at(1), face.vertices.at(2));
            std::ranges::transform(face.normal, face.normal.begin(), [](double value) { return -value; });
            face.offset = -face.offset;
        }
        return true;
    }
    return false;
}

std::uint64_t edge_key(int first, int second) {
    if (first > second) {
        std::swap(first, second);
    }
    return (static_cast<std::uint64_t>(first) << 32U) | static_cast<std::uint32_t>(second);
}

struct Distance {
    double value;
    int index;
};
int descending(const void* lhs, const void* rhs) {
    const double a = static_cast<const Distance*>(lhs)->value;
    const double b = static_cast<const Distance*>(rhs)->value;
    if (a > b) {
        return -1;
    }
    return a < b ? 1 : 0;
}

// Preserve the reference algorithm's ordered transaction for each inserted point.
// NOLINTNEXTLINE(readability-function-size)
bool hull(std::span<const float> vertices, std::vector<int>& output) {
    const std::size_t count = vertices.size() / 3;
    if (count < 4 || count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    std::vector<Point> points(count);
    // This RNG is SAF's geometric tie-break, not a security source. Keep its
    // sequence so the reference and accelerated hull select the same faces.
    // NOLINTBEGIN(clang-analyzer-security.insecureAPI.rand)
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            points[i].at(j) =
                static_cast<double>(vertices[(i * 3) + j]) +
                (0.0000001 * static_cast<double>(std::rand()) /
                 static_cast<double>(
                     RAND_MAX)); // NOLINT(clang-analyzer-security.insecureAPI.rand): match SAF geometric perturbation
        }
        points[i].at(3) = 1.0;
    }
    // NOLINTEND(clang-analyzer-security.insecureAPI.rand)
    std::array<double, 3> span{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        double low = 2.23e13;
        double high = -2.23e13;
        for (const auto& point : points) {
            low = std::min(low, point.at(axis));
            high = std::max(high, point.at(axis));
        }
        span.at(axis) = high - low;
        if (!(span.at(axis) > 0.0000001F)) {
            return false;
        }
    }
    std::array<double, 3> mean{};
    if (count > 4) {
        for (std::size_t i = 4; i < count; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                mean.at(j) += points[i].at(j);
            }
        }
        std::ranges::transform(
            mean, mean.begin(), [count](double value) { return value / static_cast<double>(count - 4); });
    }
    std::vector<Distance> order;
    order.reserve(count - 4);
    for (std::size_t i = 4; i < count; ++i) {
        double distance = 0.0;
        for (std::size_t j = 0; j < 3; ++j) {
            distance += std::pow((points[i].at(j) - mean.at(j)) / span.at(j), 2.0);
        }
        order.push_back({distance, static_cast<int>(i)});
    }
    // Same comparator and input order as SAF, including equal-distance ties.
    if (order.size() > 1) {
        std::qsort(order.data(), order.size(), sizeof(Distance), descending);
    }
    std::vector<Face> faces;
    std::vector<std::size_t> active;
    faces.reserve(count * 8);
    active.reserve(count * 2);
    std::unordered_map<std::uint64_t, std::array<std::size_t, 2>> edges;
    edges.reserve(count * 3);
    const auto attach = [&](std::size_t id) {
        const auto& face = faces[id];
        for (std::size_t j = 0; j < 3; ++j) {
            auto [entry, added] = edges.try_emplace(edge_key(face.vertices.at(j), face.vertices.at((j + 1) % 3)),
                                                    std::array<std::size_t, 2>{absent, absent});
            (void) added;
            auto& owners = entry->second;
            if (owners[0] == absent) {
                owners[0] = id;
            } else if (owners[1] == absent) {
                owners[1] = id;
            } else {
                return false;
            }
        }
        return true;
    };
    for (int opposite = 0; opposite < 4; ++opposite) {
        Triangle triangle{};
        std::size_t j = 0;
        for (int vertex = 0; vertex < 4; ++vertex) {
            if (vertex != opposite) {
                triangle.at(j++) = vertex;
            }
        }
        faces.push_back(make_face(triangle, points));
        if (!orient(faces.back(), points, static_cast<std::size_t>(opposite)) || !attach(faces.size() - 1)) {
            return false;
        }
        active.push_back(faces.size() - 1);
    }
    std::vector<double> normals;
    std::vector<double> distances;
    std::vector<std::size_t> visible;
    std::vector<std::array<int, 2>> horizon;
    for (const auto& next : order) {
        normals.clear();
        for (std::size_t id : active) {
            normals.insert(normals.end(), faces[id].normal.begin(), faces[id].normal.end());
        }
        distances.resize(active.size());
        const auto& point = points[static_cast<std::size_t>(next.index)];
        cblas_dgemm(CblasRowMajor,
                    CblasNoTrans,
                    CblasTrans,
                    1,
                    static_cast<int>(active.size()),
                    3,
                    1.0,
                    point.data(),
                    3,
                    normals.data(),
                    3,
                    0.0,
                    distances.data(),
                    static_cast<int>(active.size()));
        visible.clear();
        for (std::size_t j = 0; j < active.size(); ++j) {
            Face& face = faces[active[j]];
            face.visible = distances[j] + face.offset > 0.0;
            if (face.visible) {
                visible.push_back(active[j]);
            }
        }
        if (visible.empty()) {
            continue;
        }
        horizon.clear();
        for (std::size_t id : visible) {
            const auto& face = faces[id];
            std::vector<std::size_t> neighbors;
            for (std::size_t j = 0; j < 3; ++j) {
                const auto found = edges.find(edge_key(face.vertices.at(j), face.vertices.at((j + 1) % 3)));
                if (found == edges.end()) {
                    return false;
                }
                std::ranges::copy_if(found->second, std::back_inserter(neighbors), [&](std::size_t owner) {
                    return owner != absent && !faces[owner].visible;
                });
            }
            std::ranges::sort(neighbors);
            const auto duplicates = std::ranges::unique(neighbors);
            neighbors.erase(duplicates.begin(), duplicates.end());
            for (std::size_t neighbor : neighbors) {
                std::array<int, 2> edge{};
                std::size_t shared = 0;
                for (int vertex : faces[neighbor].vertices) {
                    if (std::ranges::find(face.vertices, vertex) != face.vertices.end()) {
                        if (shared < 2) {
                            edge.at(shared) = vertex;
                        }
                        ++shared;
                    }
                }
                if (shared == 2) {
                    horizon.push_back(edge);
                }
            }
        }
        for (std::size_t id : visible) {
            const auto& face = faces[id];
            for (std::size_t j = 0; j < 3; ++j) {
                const auto key = edge_key(face.vertices.at(j), face.vertices.at((j + 1) % 3));
                auto entry = edges.find(key);
                if (entry == edges.end()) {
                    return false;
                }
                std::ranges::replace(entry->second, id, absent);
                if (entry->second[0] == absent && entry->second[1] == absent) {
                    edges.erase(entry);
                }
            }
        }
        std::erase_if(active, [&](std::size_t id) { return faces[id].visible; });
        if (active.size() + horizon.size() > max_faces) {
            return false;
        }
        for (const auto& edge : horizon) {
            faces.push_back(make_face({edge.at(0), edge.at(1), next.index}, points));
            if (!orient(faces.back(), points, 0) || !attach(faces.size() - 1)) {
                return false;
            }
            active.push_back(faces.size() - 1);
        }
    }
    output.clear();
    for (std::size_t id : active) {
        output.insert(output.end(), faces[id].vertices.begin(), faces[id].vertices.end());
    }
    return !output.empty();
}
} // namespace

bool triangulate_hrtf(std::span<const float> directions, std::vector<float>& vertices, std::vector<int>& faces) {
    if (directions.size() % 2 != 0 || directions.size() < 8) {
        return false;
    }
    const std::size_t count = directions.size() / 2;
    vertices.resize(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        const double az = static_cast<double>(directions[i * 2]) * SAF_PId / 180.0;
        const double el = static_cast<double>(directions[(i * 2) + 1]) * SAF_PId / 180.0;
        vertices[(i * 3) + 2] = static_cast<float>(std::sin(el));
        vertices[i * 3] = static_cast<float>(std::cos(el) * std::cos(az));
        vertices[(i * 3) + 1] = static_cast<float>(std::cos(el) * std::sin(az));
    }
    if (!hull(vertices, faces)) {
        return false;
    }
    std::vector<int> filtered;
    filtered.reserve(faces.size());
    for (std::size_t i = 0; i < faces.size(); i += 3) {
        std::array<std::array<float, 3>, 3> vectors{};
        for (std::size_t j = 0; j < 3; ++j) {
            for (std::size_t k = 0; k < 3; ++k) {
                vectors.at(j).at(k) = vertices[(static_cast<std::size_t>(faces[i + j]) * 3) + k];
            }
        }
        std::array<float, 3> a{};
        std::array<float, 3> b{};
        std::array<float, 3> cross{};
        std::array<float, 3> center{};
        for (std::size_t j = 0; j < 3; ++j) {
            a.at(j) = vectors.at(1).at(j) - vectors.at(0).at(j);
            b.at(j) = vectors.at(2).at(j) - vectors.at(1).at(j);
            center.at(j) = (vectors.at(0).at(j) + vectors.at(1).at(j) + vectors.at(2).at(j)) / 3.0F;
        }
        cross = {(a.at(1) * b.at(2)) - (a.at(2) * b.at(1)),
                 (a.at(2) * b.at(0)) - (a.at(0) * b.at(2)),
                 (a.at(0) * b.at(1)) - (a.at(1) * b.at(0))};
        const float dot = (cross[0] * center.at(0)) + (cross[1] * center.at(1)) + (cross[2] * center.at(2));
        if (!(std::acos(std::clamp(dot, -0.99999999F, 0.99999999F)) < SAF_PI / 2.0F)) {
            continue;
        }
        bool acceptable = true;
        for (std::size_t j = 0; j < 3; ++j) {
            const auto& first = vectors.at(j);
            const auto& second = vectors.at((j + 1) % 3);
            const float angle = std::acos((first[0] * second[0]) + (first[1] * second[1]) + (first[2] * second[2]));
            acceptable = acceptable && angle < 180.0F * SAF_PI / 180.0F;
        }
        if (acceptable) {
            filtered.insert(filtered.end(),
                            faces.begin() + static_cast<std::ptrdiff_t>(i),
                            faces.begin() + static_cast<std::ptrdiff_t>(i + 3));
        }
    }
    faces = std::move(filtered);
    return !faces.empty();
}
} // namespace mradm::binaural_internal
