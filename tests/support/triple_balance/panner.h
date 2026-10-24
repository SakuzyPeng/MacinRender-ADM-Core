#pragma once

#include <string_view>
#include <vector>

#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm::triple_balance {

// Reference 7.1.4/9.1.6 gains and the self-defined 22.2 extension. Output order
// follows the project layouts; Objects never feed either LFE channel.
[[nodiscard]] Result<std::vector<float>>
point_gains(const SceneBlockPosition& position, float gain, std::string_view layout_id);

} // namespace mradm::triple_balance
