#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/options.h"
#include "adm/scene.h"

namespace mradm::triple_balance {
[[nodiscard]] std::optional<std::size_t> bed_channel(const SceneDirectSpeakersBlock& block);
[[nodiscard]] Result<void> validate_bed(const SceneObject& object, const SceneInfo& info);
[[nodiscard]] Result<std::vector<float>> bed_gains(const SceneDirectSpeakersBlock& block,
                                                   std::string_view layout,
                                                   LfeRoutingMode lfe_mode = LfeRoutingMode::direct);
[[nodiscard]] float bed_user_gain(const SceneDirectSpeakersBlock& block);
} // namespace mradm::triple_balance
