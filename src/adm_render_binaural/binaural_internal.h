#pragma once

// Internal, NON-installed header shared between the binaural renderer implementation and the
// test-only interpolation probe. It exposes the module-private HRTF state and the core
// interpolation entry points so that binaural_test_probe.cpp can be compiled as a separate,
// test-only translation unit — keeping probe_hrtf_interpolation() out of the production library
// and the shipped mradm binary entirely (no macro gating, no per-platform release tweaks needed).
//
// This header carries SAF's float_complex, which is allowed here: it lives under
// src/adm_render_binaural/ and is never installed, so it does not cross the ADR-0003 target
// boundary (SAF types stay inside the binaural module).

#include <cstdint>
#include <memory>
#include <saf_utility_complex.h>
#include <string>
#include <vector>

namespace mradm::binaural_internal {

inline constexpr int k_n_ears = 2;

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
    // HRTFs in frequency domain; FLAT: [n_bands × k_n_ears × num_dirs]. compute_hrtf_into()
    // interpolates magnitude and phase from these separately (see there) to avoid the ITD-comb of
    // direct complex weighting at off-grid directions.
    std::vector<float_complex> hrtf_fd;
    // Compressed VBAP table: amplitude-normalised gains + direction indices per grid point.
    std::vector<float> vbap_gains; // FLAT: [N_gtable × 3]
    std::vector<int> vbap_dirs;    // FLAT: [N_gtable × 3]
    // Time-domain HRTFs and grid for saf_spreader mode.
    std::vector<float> hrtf_td;       // [num_dirs × k_n_ears × hrir_len]
    std::vector<float> grid_dirs_deg; // [num_dirs × 2]
    int sample_rate{0};

    BinauralState() = default;
    BinauralState(const BinauralState&) = delete;
    BinauralState& operator=(const BinauralState&) = delete;
};
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)

// Built-in SAF KEMAR HRTF dataset.
HrtfDataset built_in_kemar_dataset();

// Build the pre-computed state (frequency-domain HRTFs + compressed VBAP grid) once.
// Returns nullptr on VBAP triangulation failure.
std::unique_ptr<BinauralState> build_binaural_state(HrtfDataset dataset, std::uint64_t block_size);

// Quantised (az,el) → flat grid index into the compressed VBAP table.
int vbap_grid_idx(float az_deg, float el_deg);

// Interpolate the HRTF at (az,el) into out (magnitude/phase split; see definition).
void compute_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<float_complex>& out);

} // namespace mradm::binaural_internal
