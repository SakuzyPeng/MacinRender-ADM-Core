#include "hrtf_grid.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <list>
#include <mutex>


namespace mradm::binaural_internal {
namespace {
constexpr std::size_t cache_budget = std::size_t{16} * 1024U * 1024U;
constexpr std::size_t cache_entries = 8;
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

std::shared_ptr<const HrtfGrid> build_hrtf_grid(std::span<const float> directions) {
    auto result = HrtfGrid::create(directions);
    if (!result) {
        return {};
    }
    return std::make_shared<HrtfGrid>(std::move(*result));
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
