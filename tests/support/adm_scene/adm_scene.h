#pragma once

// The C++ AdmScene the tests and the frozen references read (PCM consistency 5.6 C): production
// keeps only the Rust render scene, so this rebuilds the former public scene from its view and
// overlay, field by field as RenderScene::scene() did.
#include <string>

#include "adm/errors.h"
#include "adm/io.h"
#include "adm/render.h"
#include "adm/renderer.h"
#include "adm/scene.h"

namespace mradm {

// A render plan with the AdmScene of its Rust scene (RenderPlan::scene until 5.6 C). Backends read
// only scene_handle; `scene` is for the test's own expectations and the frozen references.
struct ScenePlan : RenderPlan {
    AdmScene scene;
};

// The AdmScene of an imported Rust scene; `path` fills info.file_path.
[[nodiscard]] Result<AdmScene> adm_scene(const detail::RenderSceneHandle& handle, const std::string& path = {});
// PreparedScene::scene until 5.6 C; throws std::runtime_error on a rejected view.
[[nodiscard]] AdmScene adm_scene(const PreparedScene& scene);

} // namespace mradm

namespace mradm::io {

class RenderScene;

// The former SDK import (adm/io.h until 5.6 C): the Rust import, then adm_scene.
Result<AdmScene> import_scene(const std::string& path);
Result<AdmScene> import_scene(const std::string& path, const SceneImportOptions& options);

// RenderScene::scene() until 5.6 C, without info.file_path; throws std::runtime_error on a rejected view.
[[nodiscard]] AdmScene adm_scene(const RenderScene& scene);

} // namespace mradm::io
