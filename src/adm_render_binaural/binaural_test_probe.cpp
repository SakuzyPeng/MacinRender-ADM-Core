// Test-only translation unit. Implements the HRTF-interpolation invariant probe declared in
// binaural_test_probe.h, using the module-internal HRTF state/entry points from binaural_internal.h.
// This file is compiled ONLY into the binaural unit-test target — never into the production
// ADMRenderBinaural library or the shipped mradm binary (regardless of platform or BUILD_TESTS).

#include "binaural_test_probe.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include "binaural_internal.h"

namespace mradm::binaural_detail {

using namespace binaural_internal;

HrtfInterpProbe probe_hrtf_interpolation(float az_deg, float el_deg, int ear) {
    HrtfInterpProbe out;
    if (ear < 0 || ear >= k_n_ears) {
        return out;
    }
    // Build the built-in KEMAR state once and reuse it across probe calls — building the VBAP grid
    // is expensive and the dataset is fixed, so rebuilding per call would dominate the fixture time.
    constexpr std::uint64_t k_probe_block = 512U;
    static const std::unique_ptr<BinauralState> s_state = build_binaural_state(built_in_kemar_dataset(), k_probe_block);
    if (!s_state) {
        return out;
    }
    const BinauralState& bs = *s_state;
    const auto nd = static_cast<std::size_t>(bs.num_dirs);
    const auto e = static_cast<std::size_t>(ear);
    const auto g = static_cast<std::size_t>(vbap_grid_idx(az_deg, el_deg));
    const auto gbase = g * 3U;

    // Identify the dominant VBAP direction and whether this resolves to a single grid point.
    std::size_t best_k = 0;
    for (std::size_t k = 1; k < 3U; ++k) {
        if (bs.vbap_gains[gbase + k] > bs.vbap_gains[gbase + best_k]) {
            best_k = k;
        }
    }
    out.grid_point = bs.vbap_gains[gbase + best_k] >= 0.99F;
    const auto dom_dir = static_cast<std::size_t>(bs.vbap_dirs[gbase + best_k]);

    // Run the real interpolation path.
    std::vector<float_complex> hrtf;
    compute_hrtf_into(bs, az_deg, el_deg, hrtf);

    const auto n_bands = static_cast<std::size_t>(bs.n_bands);
    out.freqs.resize(n_bands);
    out.interp_mag.resize(n_bands);
    out.nbr_min_mag.resize(n_bands);
    out.nbr_max_mag.resize(n_bands);
    out.grid_raw_mag.resize(n_bands);
    const auto fs = static_cast<float>(bs.sample_rate);
    const auto fft_size = static_cast<float>(bs.fft_size);
    const auto ears = static_cast<std::size_t>(k_n_ears);
    for (std::size_t b = 0; b < n_bands; ++b) {
        out.freqs[b] = static_cast<float>(b) * fs / fft_size;
        out.interp_mag[b] = std::abs(hrtf[(b * ears) + e]);
        out.grid_raw_mag[b] = std::abs(bs.hrtf_fd[(b * ears * nd) + (e * nd) + dom_dir]);
        float mn = std::numeric_limits<float>::max();
        float mx = 0.0F;
        for (std::size_t k = 0; k < 3U; ++k) {
            if (bs.vbap_gains[gbase + k] <= 0.0F) {
                continue;
            }
            const auto dir = static_cast<std::size_t>(bs.vbap_dirs[gbase + k]);
            const float m = std::abs(bs.hrtf_fd[(b * ears * nd) + (e * nd) + dir]);
            mn = std::min(mn, m);
            mx = std::max(mx, m);
        }
        out.nbr_min_mag[b] = mn;
        out.nbr_max_mag[b] = mx;
    }
    out.valid = true;
    return out;
}

} // namespace mradm::binaural_detail
