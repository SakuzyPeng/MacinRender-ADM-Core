#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "adm/errors.h"

#include "hrtf.h"
#include "routing_ffi.h"

namespace mradm::binaural_internal {

inline constexpr int k_n_ears = 2;

// The prepared HRTF state of Rust mradm-render `binaural_hrtf` (PCM consistency 4.7 A): dataset
// loading, preparation, the geometry cache and the Scene stream cache run in Rust. The members
// borrow the Rust state, which the object keeps alive; the filter bank shares its spectra.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct HrtfState {
    // The file renderer's state: the built-in set or a SOFA file at input_rate, for
    // block-frame OLA blocks.
    [[nodiscard]] static Result<std::shared_ptr<const HrtfState>>
    prepare(const std::optional<std::filesystem::path>& sofa, std::uint32_t input_rate, std::uint64_t block);
    // The Scene stream renderer's cached state at sample_rate; an empty path selects the built-in
    // set. It has no HRIRs or directions.
    [[nodiscard]] static Result<std::shared_ptr<const HrtfState>> prepare_live(const std::filesystem::path& sofa,
                                                                               std::uint32_t sample_rate);

    std::string dataset_name;
    int sample_rate{0};
    int num_dirs{0};
    int hrir_len{0};
    int fft_size{0};
    int n_bands{0};
    int overlap_len{0};
    dsp::HrtfFilters filters;
    std::span<const float> hrtf_td;       // [num_dirs × k_n_ears × hrir_len]
    std::span<const float> grid_dirs_deg; // [num_dirs × 2]
    std::shared_ptr<MradmBinauralHrtfHandle> handle;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

} // namespace mradm::binaural_internal
