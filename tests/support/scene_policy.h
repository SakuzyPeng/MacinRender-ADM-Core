#pragma once

#include <string_view>

#include "adm/errors.h"

#include "../../src/adm_io/scene_import_internal.h"
#include "engine_result.h"
#include "scene_handle.h"

namespace mradm::test {

// A semantic policy applied to an imported scene as a preview session applies it (mradm-engine
// `render`, PCM consistency 5.2 C2).
[[nodiscard]] inline Result<void> apply_policy(io::RenderScene& scene, std::string_view json) {
    const render_common::EngineOutcome applied([&](MradmEngineResult** out, MradmEngineStatus* status) {
        return mradm_engine_apply_policy(
            scene.handle()->raw, 1U, render_common::engine_text(json), 0U, {}, {}, out, status);
    });
    if (auto refreshed = scene.refresh(); !refreshed) {
        return refreshed;
    }
    if (!applied.ok()) {
        return tl::unexpected{applied.error()};
    }
    return {};
}

} // namespace mradm::test
