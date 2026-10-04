#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "hrtf.h"

namespace mradm::binaural_internal {

// Geometry only: independent of the HRIR samples, FFT size and design rate.
// Rust retains the shared table; diagnostics/tests request explicit snapshots.
using HrtfGrid = dsp::HrtfGrid;

// Fixed one-degree grid with amplitude-normalized sparse Rust VBAP weights.
[[nodiscard]] std::shared_ptr<const HrtfGrid> prepare_hrtf_grid(std::span<const float> directions);
[[nodiscard]] std::shared_ptr<const HrtfGrid> build_hrtf_grid(std::span<const float> directions);

// Current memory retained by the geometry cache.
std::size_t hrtf_grid_cache_bytes();

} // namespace mradm::binaural_internal
