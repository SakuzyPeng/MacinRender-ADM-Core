// Frozen from 1972817 for migration tests only.
#pragma once

#include <array>
#include <cstdint>

#include "adm/errors.h"

namespace mradm::triple_balance_legacy {

using SizeGains = std::array<float, 11>;

// Internal coordinates: left/front/bed plane is (0,0,0).
// The self-defined 22.2 extension additionally accepts negative Z down to -1.
struct SizePosition {
    float x{};
    float y{};
    float z{};
};

struct QuantizedSizeParameters {
    std::array<int32_t, 3> xyz{};
    int32_t size{};
};

struct SizeMixGains {
    float direct{1.0F};
    SizeGains spread{};
};

// Quantization is a separate boundary, so already quantized callback fixtures
// can exercise the spatial function without an additional coordinate mapping.
[[nodiscard]] Result<QuantizedSizeParameters> quantize_size_parameters(SizePosition position, float size);
[[nodiscard]] Result<SizeGains> raw_size_gains(const QuantizedSizeParameters& parameters);
[[nodiscard]] SizeMixGains mix_size_gains(const SizeGains& raw, float effective_size) noexcept;

} // namespace mradm::triple_balance_legacy
