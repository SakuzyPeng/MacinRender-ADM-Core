#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace mradm::binaural_internal {

// Geometry only: independent of the HRIR samples, FFT size and design rate.
// Read by renderer and test translation units.
// cppcheck-suppress-begin unusedStructMember
struct HrtfGrid {
    std::vector<float> gains;
    std::vector<int> directions;
    [[nodiscard]] std::size_t bytes() const noexcept;
};
// cppcheck-suppress-end unusedStructMember

// Fixed one-degree grid with amplitude-normalized sparse Rust VBAP weights.
[[nodiscard]] std::shared_ptr<const HrtfGrid> prepare_hrtf_grid(std::span<const float> directions);
[[nodiscard]] std::shared_ptr<const HrtfGrid> build_hrtf_grid(std::span<const float> directions);

// Current memory retained by the geometry cache.
std::size_t hrtf_grid_cache_bytes();

} // namespace mradm::binaural_internal
