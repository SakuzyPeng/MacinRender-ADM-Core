#pragma once

#include <string>

#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm {
struct RenderPlan;
namespace room_compat {

// Normalize only the private compatibility copy. Source/policy scenes and other
// renderers retain their ADM semantics. report is populated on rejection too.
[[nodiscard]] Result<AdmScene> prepare_semantics(const RenderPlan& plan, std::string& report);
void publish_semantics(const RenderPlan& plan, const std::string& report, const Error* error = nullptr);
[[nodiscard]] float user_output_gain(const SceneObject& object);

} // namespace room_compat
} // namespace mradm
