#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#include "decimal_ffi.h"

// Locale-independent, correctly rounded decimal parameters (PCM consistency 3.5). Unlike strtod /
// strtold the result does not depend on the C library, its long double or the process locale.
namespace mradm::dsp {

// A finite float or double parsed from decimal text, rounded once to T; nullopt otherwise.
template <typename T> [[nodiscard]] std::optional<T> parse_decimal(std::string_view text) noexcept {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);
    double value = 0.0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): UTF-8 bytes for the C boundary.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    if (mradm_dsp_parse_decimal(bytes, text.size(), std::is_same_v<T, float> ? 1U : 0U, &value) != 0) {
        return std::nullopt;
    }
    return static_cast<T>(value);
}

} // namespace mradm::dsp
