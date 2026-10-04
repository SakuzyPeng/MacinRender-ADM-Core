#pragma once

// Internal, NON-installed header shared between the binaural renderer implementation and the
// test-only interpolation probe. It exposes the module-private HRTF state and the core
// interpolation entry points so that binaural_test_probe.cpp can be compiled as a separate,
// test-only translation unit — keeping probe_hrtf_interpolation() out of the production library
// and the shipped mradm binary entirely (no macro gating, no per-platform release tweaks needed).
//
// Complex samples and owned FFT handles use the private project DSP boundary.
// This header is not installed and no Rust/third-party type enters the public ABI.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adm/errors.h"

#include "dsp.h"
#include "hrtf_grid.h"

namespace mradm::binaural_internal {

inline constexpr int k_n_ears = 2;

// cppcheck-suppress-begin unusedStructMember
struct HrtfDataset {
    std::string name;
    int sample_rate{0};
    int hrir_len{0};
    int num_dirs{0};
    std::vector<float> dirs_deg; // FLAT: [num_dirs × 2] az/el in degrees
    std::vector<float> hrirs;    // FLAT: [num_dirs × 2 × hrir_len]
};

// NOLINTBEGIN(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)
struct BinauralState {
    int num_dirs{0};
    int hrir_len{0};
    int fft_size{0};
    int n_bands{0};
    int overlap_len{0};
    std::string dataset_name;
    // Immutable Rust spectra and optional live magnitude cache. Each bank
    // shares the Rust geometry allocation with the bounded C++ grid cache.
    dsp::HrtfFilters filters;
    // Compressed VBAP table: amplitude-normalised gains + direction indices per grid point.
    std::shared_ptr<const HrtfGrid> grid;
    // Time-domain HRTFs and grid for saf_spreader mode.
    std::vector<float> hrtf_td;       // [num_dirs × k_n_ears × hrir_len]
    std::vector<float> grid_dirs_deg; // [num_dirs × 2]
    int sample_rate{0};

    BinauralState() = default;
    BinauralState(const BinauralState&) = delete;
    BinauralState& operator=(const BinauralState&) = delete;
};
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)
// cppcheck-suppress-end unusedStructMember

// Built-in KEMAR HRTF dataset (attributed immutable data, independent of SAF).
HrtfDataset built_in_kemar_dataset();

// Load a user SOFA dataset. This remains module-private; live Scene rendering uses the
// same validation and reader path as the file renderer.
Result<HrtfDataset> load_sofa_dataset(const std::filesystem::path& path, std::uint32_t input_sample_rate);

// Build the pre-computed state (frequency-domain HRTFs + compressed VBAP grid) once.
// Geometry/filter preparation errors retain their Result error code.
Result<std::unique_ptr<BinauralState>>
build_binaural_state(HrtfDataset dataset, std::uint64_t block_size, bool cache_magnitudes = false);

// Quantised (az,el) → flat grid index into the compressed VBAP table.
int vbap_grid_idx(float az_deg, float el_deg);

// Interpolate the HRTF at (az,el) into out (magnitude/phase split; see definition).
void compute_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<dsp::Complex>& out);

// Live motion interpolates the complex responses of adjacent one-degree cells.
// Integer directions retain the existing magnitude-preserving response; moving
// within or across a cell is continuous, including the +/-180 degree seam.
void compute_continuous_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<dsp::Complex>& out);

} // namespace mradm::binaural_internal
