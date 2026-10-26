#pragma once

// Internal, NON-installed header. Test-only probe of the VBAP HRTF interpolation, built on the
// in-process built-in KEMAR dataset. Returns plain magnitudes (no SAF types cross the boundary) so
// unit tests can assert the invariants that constrain the magnitude/phase-split interpolation in
// compute_hrtf_into(): grid-point identity, the convex-combination bound that prevents fabricated
// comb notches, and lateral head-shadow preservation. This header is deliberately kept out of the
// public include/ tree — it is not part of the stable C++ API.

#include <vector>

namespace mradm::binaural_detail {

struct HrtfInterpProbe {
    bool valid{false};               // false if the KEMAR state could not be built
    bool grid_point{false};          // VBAP resolves to a single measured direction (gain ≈ 1)
    std::vector<float> freqs;        // per-band centre frequency (Hz), n_bands
    std::vector<float> interp_mag;   // interpolated |HRTF| per band (what compute_hrtf_into emits)
    std::vector<float> nbr_min_mag;  // band-wise min |HRTF| over the contributing measured dirs
    std::vector<float> nbr_max_mag;  // band-wise max |HRTF| over the contributing measured dirs
    std::vector<float> grid_raw_mag; // raw |HRTF| of the dominant measured dir (grid-point identity)
};

// One ear per call. The built-in KEMAR BinauralState is built once and cached across calls.
HrtfInterpProbe probe_hrtf_interpolation(float az_deg, float el_deg, int ear);

} // namespace mradm::binaural_detail
