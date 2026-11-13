#pragma once

#include <string_view>

namespace mradm {

[[nodiscard]] std::string_view version() noexcept;
[[nodiscard]] std::string_view build_profile() noexcept;

} // namespace mradm
