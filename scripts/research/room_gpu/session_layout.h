#pragma once
#include <array>
#include <cstddef>

namespace room_gpu_layout {
enum Field : unsigned { header, ranges, updates, tasks, groups, info, input, fields };
struct Upload {
    std::array<std::size_t, fields> offsets{}, capacities{};
    std::size_t bytes = 0;
    explicit Upload(const std::array<std::size_t, fields>& lengths) : capacities(lengths) {
        for (unsigned i = 0; i < fields; ++i) {
            offsets[i] = bytes;
            bytes += (lengths[i] + 63) & ~std::size_t(63);
        }
    }
};
} // namespace room_gpu_layout
