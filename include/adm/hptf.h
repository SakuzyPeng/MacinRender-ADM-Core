#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"

namespace mradm {

inline constexpr std::size_t k_hptf_max_bands = 32;

enum class HptfBandType : std::uint8_t {
    peaking = 0,
    low_shelf = 1,
    high_shelf = 2,
    low_pass = 3,
    high_pass = 4,
    band_pass = 5,
    notch = 6,
};

// Sample-rate-independent editor parameters. Disabled bands remain in the profile.
struct HptfBand {
    HptfBandType type{HptfBandType::peaking};
    bool enabled{true};
    double fc_hz{1000.0};
    double gain_db{0.0};
    double q{0.707};
};

struct HptfProfile {
    double preamp_db{0.0};
    std::vector<HptfBand> bands;
    std::string name; // Optional display metadata; not interpreted as a filename.
};

// Pure in-memory AutoEq import. No device, filesystem or sample rate is needed.
// The returned value owns its parameters and can be edited before applying it.
// Implemented by ADMRenderCommon (also linked by ADMEngine / ADMCAPI).
[[nodiscard]] Result<HptfProfile> parse_hptf_parametric_eq(std::string_view text);

} // namespace mradm
