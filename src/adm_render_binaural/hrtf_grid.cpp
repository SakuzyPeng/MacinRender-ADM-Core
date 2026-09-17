#include "hrtf_grid.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <numeric>
#include <saf_utilities.h>
#include <saf_vbap.h>
#include <utility>

#include "consistency_trace.h"

namespace mradm::binaural_internal {
namespace {
constexpr std::size_t azimuths = 361;
constexpr std::size_t elevations = 181;
constexpr std::size_t grid_points = azimuths * elevations;
constexpr std::size_t cache_budget = std::size_t{16} * 1024U * 1024U;
constexpr std::size_t cache_entries = 8;
struct Free {
    // SAF returns malloc-owned buffers; this is their unique_ptr deleter.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    void operator()(void* pointer) const noexcept { std::free(pointer); }
};
struct Query {
    std::array<float, 3> direction{};
    std::size_t index{0};
};
struct Node {
    std::array<float, 3> low{};
    std::array<float, 3> high{};
    std::size_t begin{0};
    std::size_t end{0};
    std::size_t left{0};
    std::size_t right{0};
};
// Module-private tree is immutable after its one-time construction.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct QueryTree {
    std::vector<Query> queries;
    std::vector<Node> nodes;
    QueryTree() {
        queries.reserve(grid_points);
        nodes.reserve(grid_points / 4);
        for (std::size_t i = 0; i < grid_points; ++i) {
            const float az = (-180.0F + static_cast<float>(i % azimuths)) * SAF_PI / 180.0F;
            // Integer quotient intentionally identifies the elevation row.
            // NOLINTNEXTLINE(bugprone-integer-division)
            const float el = (-90.0F + static_cast<float>(i / azimuths)) * SAF_PI / 180.0F;
            queries.push_back({{std::cos(az) * std::cos(el), std::sin(az) * std::cos(el), std::sin(el)}, i});
        }
        build(0, queries.size());
    }
    // Balanced fixed-size tree: recursion depth is at most 13.
    // NOLINTNEXTLINE(misc-no-recursion)
    std::size_t build(std::size_t begin, std::size_t end) {
        Node node;
        node.begin = begin;
        node.end = end;
        node.low.fill(std::numeric_limits<float>::infinity());
        node.high.fill(-std::numeric_limits<float>::infinity());
        for (std::size_t i = begin; i < end; ++i) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                node.low.at(axis) = std::min(node.low.at(axis), queries[i].direction.at(axis));
                node.high.at(axis) = std::max(node.high.at(axis), queries[i].direction.at(axis));
            }
        }
        const std::size_t id = nodes.size();
        nodes.push_back(node);
        if (end - begin > 16) {
            std::size_t axis = 0;
            for (std::size_t i = 1; i < 3; ++i) {
                if (node.high.at(i) - node.low.at(i) > node.high.at(axis) - node.low.at(axis)) {
                    axis = i;
                }
            }
            const std::size_t middle = begin + ((end - begin) / 2);
            std::nth_element(queries.begin() + static_cast<std::ptrdiff_t>(begin),
                             queries.begin() + static_cast<std::ptrdiff_t>(middle),
                             queries.begin() + static_cast<std::ptrdiff_t>(end),
                             [axis](const Query& a, const Query& b) {
                                 return a.direction.at(axis) == b.direction.at(axis)
                                            ? a.index < b.index
                                            : a.direction.at(axis) < b.direction.at(axis);
                             });
            nodes[id].left = build(begin, middle);
            nodes[id].right = build(middle, end);
        }
        return id;
    }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

class SparseBuilder {
  public:
    SparseBuilder(const QueryTree& tree, HrtfGrid& grid, std::size_t measurements)
        : tree_(tree), grid_(grid), measurements_(measurements), assigned_(grid_points, false) {
        remaining_.reserve(tree.nodes.size());
        std::ranges::transform(
            tree.nodes, std::back_inserter(remaining_), [](const Node& node) { return node.end - node.begin; });
        grid_.gains.assign(grid_points * 3, 0.0F);
        grid_.directions.assign(grid_points * 3, 0);
    }
    void face(std::span<const float, 9> matrix, std::span<const int, 3> indices) { visit(0, matrix, indices); }

  private:
    static bool outside(const Node& node, std::span<const float, 9> matrix) {
        for (std::size_t row = 0; row < 3; ++row) {
            double upper = 0.0;
            double magnitude = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double coefficient = matrix[(row * 3) + axis];
                upper += coefficient * static_cast<double>(coefficient < 0 ? node.low.at(axis) : node.high.at(axis));
                magnitude +=
                    std::abs(coefficient) * std::max(std::abs(node.low.at(axis)), std::abs(node.high.at(axis)));
            }
            // The leaf uses SAF's own dot product. This interval only rejects
            // boxes separated even after allowing for all float rounding.
            const double rounding = 8.0 * std::numeric_limits<float>::epsilon() * (magnitude + 1.0);
            if (std::isfinite(upper) && upper + rounding <= -0.001) {
                return true;
            }
        }
        return false;
    }
    bool assign(const Query& query, std::span<const float, 9> matrix, std::span<const int, 3> indices) {
        std::array<float, 3> gains{};
        float minimum = 2.23e13F;
        float norm = 0.0F;
        for (std::size_t i = 0; i < 3; ++i) {
            std::array<float, 3> row{matrix[i * 3], matrix[(i * 3) + 1], matrix[(i * 3) + 2]};
            auto direction = query.direction;
            utility_svvdot(row.data(), direction.data(), 3, &gains.at(i));
            minimum = minimum < gains.at(i) ? minimum : gains.at(i);
            norm += std::pow(gains.at(i), 2.0F);
        }
        if (!(minimum > -0.001)) {
            return false;
        }
        norm = std::sqrt(norm);
        std::array<std::pair<int, float>, 3> sorted{};
        for (std::size_t i = 0; i < 3; ++i) {
            sorted.at(i) = {indices[i], gains.at(i) / norm};
        }
        std::ranges::sort(sorted, {}, &std::pair<int, float>::first);
        // Preserve both SAF normalisations, including dummy vertices, before
        // the existing amplitude normalisation of real measurement indices.
        norm = std::accumulate(sorted.begin(), sorted.end(), 0.0F, [](float sum, const auto& item) {
            return sum + std::pow(item.second, 2.0F);
        });
        norm = std::sqrt(norm);
        std::size_t used = 0;
        float sum = 0.0F;
        for (const auto& [index, gain] : sorted) {
            const float value = gain / norm > 0.0F ? gain / norm : 0.0F;
            if (index >= 0 && std::cmp_less(index, measurements_) && value > 0.0000001F) {
                grid_.directions[(query.index * 3) + used] = index;
                grid_.gains[(query.index * 3) + used] = value;
                sum += value;
                ++used;
            }
        }
        for (std::size_t i = 0; i < used; ++i) {
            grid_.gains[(query.index * 3) + i] /= sum;
        }
        assigned_[query.index] = true;
        return true;
    }
    // Same bounded depth as QueryTree::build.
    // NOLINTNEXTLINE(misc-no-recursion)
    std::size_t visit(std::size_t id, std::span<const float, 9> matrix, std::span<const int, 3> indices) {
        if (remaining_[id] == 0 || outside(tree_.nodes[id], matrix)) {
            return 0;
        }
        const Node& node = tree_.nodes[id];
        std::size_t count = 0;
        if (node.left == 0) {
            for (std::size_t i = node.begin; i < node.end; ++i) {
                const Query& query = tree_.queries[i];
                if (!assigned_[query.index] && assign(query, matrix, indices)) {
                    ++count;
                }
            }
        } else {
            count = visit(node.left, matrix, indices) + visit(node.right, matrix, indices);
        }
        remaining_[id] -= count;
        return count;
    }
    const QueryTree& tree_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): scoped builder borrow
    HrtfGrid& grid_;        // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): scoped builder borrow
    std::size_t measurements_;
    std::vector<bool> assigned_;
    std::vector<std::size_t> remaining_;
};

struct CacheEntry {
    std::uint64_t hash{0};
    std::vector<float> key;
    std::shared_ptr<const HrtfGrid> grid;
    std::size_t bytes{0};
};
// Module-private aggregate; find/prepare always acquire its corresponding locks.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Cache {
    std::mutex mutex;
    // Serialises cold builds, also coalescing concurrent misses without an
    // unbounded map of in-flight work. No renderer/audio callback takes it.
    std::mutex build_mutex;
    std::list<CacheEntry> entries;
    std::size_t bytes{0};
    std::shared_ptr<const HrtfGrid> find(std::span<const float> key, std::uint64_t hash) {
        const std::lock_guard lock(mutex);
        const auto found = std::ranges::find_if(entries, [&](const CacheEntry& entry) {
            return entry.hash == hash && entry.key.size() == key.size() &&
                   std::memcmp(entry.key.data(), key.data(), key.size_bytes()) == 0;
        });
        if (found == entries.end()) {
            return {};
        }
        auto result = found->grid;
        entries.splice(entries.end(), entries, found);
        return result;
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)
Cache& cache() {
    static Cache value;
    return value;
}
} // namespace

std::size_t HrtfGrid::bytes() const noexcept {
    return sizeof(*this) + (gains.capacity() * sizeof(float)) + (directions.capacity() * sizeof(int));
}

std::shared_ptr<const HrtfGrid> build_hrtf_grid(std::span<const float> directions) {
    if (directions.size() < 8 || directions.size() % 2 != 0 ||
        !std::ranges::all_of(directions, [](float v) { return std::isfinite(v); })) {
        return {};
    }
    std::vector<float> triangulation(directions.begin(), directions.end());
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
    std::vector<float> vertices;
    std::vector<int> faces;
    if (!triangulate_hrtf(triangulation, vertices, faces)) {
        float* old_vertices = nullptr;
        int* old_faces = nullptr;
        int vertex_count = 0;
        int face_count = 0;
        findLsTriplets(triangulation.data(),
                       static_cast<int>(triangulation.size() / 2),
                       1,
                       &old_vertices,
                       &vertex_count,
                       &old_faces,
                       &face_count);
        const std::unique_ptr<float, Free> vertices_guard(old_vertices);
        const std::unique_ptr<int, Free> faces_guard(old_faces);
        if (old_vertices == nullptr || old_faces == nullptr || face_count <= 0) {
            return {};
        }
        vertices.assign(old_vertices, old_vertices + (static_cast<std::ptrdiff_t>(vertex_count) * 3));
        faces.assign(old_faces, old_faces + (static_cast<std::ptrdiff_t>(face_count) * 3));
    }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    consistency::dump("binaural.02a-triangulation-vertices.f32", vertices);
    consistency::dump("binaural.02b-triangulation-faces.i32", faces);
#endif
    float* inverses = nullptr;
    invertLsMtx3D(vertices.data(), faces.data(), static_cast<int>(faces.size() / 3), &inverses);
    const std::unique_ptr<float, Free> inverses_guard(inverses);
    if (inverses == nullptr) {
        return {};
    }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    consistency::dump("binaural.02c-inverse-matrices.f32", std::span<const float>(inverses, (faces.size() / 3) * 9));
#endif
    static const QueryTree tree;
    auto result = std::make_shared<HrtfGrid>();
    SparseBuilder builder(tree, *result, directions.size() / 2);
    for (std::size_t i = 0; i < faces.size() / 3; ++i) {
        builder.face(std::span<const float, 9>(inverses + (i * 9), 9),
                     std::span<const int, 3>(faces.data() + (i * 3), 3));
    }
    return result;
}

std::shared_ptr<const HrtfGrid> prepare_hrtf_grid(std::span<const float> directions) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (float value : directions) {
        hash ^= std::bit_cast<std::uint32_t>(value);
        hash *= 1099511628211ULL;
    }
    Cache& store = cache();
    if (auto existing = store.find(directions, hash)) {
        return existing;
    }
    const std::lock_guard build_lock(store.build_mutex);
    if (auto existing = store.find(directions, hash)) {
        return existing;
    }
    auto grid = build_hrtf_grid(directions);
    if (!grid) {
        return {};
    }
    std::vector<float> key(directions.begin(), directions.end());
    const std::size_t bytes = sizeof(CacheEntry) + (key.capacity() * sizeof(float)) + grid->bytes();
    if (bytes <= cache_budget) {
        const std::lock_guard lock(store.mutex);
        while (!store.entries.empty() &&
               (store.entries.size() >= cache_entries || store.bytes + bytes > cache_budget)) {
            store.bytes -= store.entries.front().bytes;
            store.entries.pop_front();
        }
        store.entries.push_back({hash, std::move(key), grid, bytes});
        store.bytes += bytes;
    }
    return grid;
}

std::size_t hrtf_grid_cache_bytes() {
    Cache& store = cache();
    const std::lock_guard lock(store.mutex);
    return store.bytes;
}
} // namespace mradm::binaural_internal
