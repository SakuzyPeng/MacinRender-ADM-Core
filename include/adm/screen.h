#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

#include "adm/errors.h"

namespace mradm {
struct AdmScene;

// ADM screen coordinates: polar centre is azimuth/elevation/distance and width
// is angular degrees; Cartesian centre is x/right, y/front, z/up and width is X.
// cppcheck-suppress-begin unusedStructMember
struct ScreenGeometry {
    bool cartesian{false};
    double aspect_ratio{1.78};
    std::array<double, 3> centre{0.0, 0.0, 1.0};
    double width{58.0};
    bool operator==(const ScreenGeometry&) const = default;
};
struct ScreenConfig {
    std::optional<ScreenGeometry> reference;
    std::optional<ScreenGeometry> reproduction;
};
// cppcheck-suppress-end unusedStructMember

[[nodiscard]] Result<ScreenConfig> parse_screen_config_json(std::string_view source);
[[nodiscard]] Result<ScreenConfig> load_screen_config(const std::filesystem::path& path);
[[nodiscard]] Result<void> validate_screen_config(const ScreenConfig& config);
[[nodiscard]] Result<ScreenGeometry>
resolve_reference_screen(const AdmScene& scene, std::string_view object_id, const ScreenConfig& config);
} // namespace mradm
