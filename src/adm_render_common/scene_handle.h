#pragma once

#include "adm/render.h"

#include "../adm_metadata/scene_ffi.h"

namespace mradm::detail {
// The imported Rust render scene (mradm-scene) behind RenderPlan::scene_handle; the Rust
// loudspeaker backends read it instead of the AdmScene copy (PCM consistency 4.5).
struct RenderSceneHandle {
    explicit RenderSceneHandle(MradmSceneHandle* scene) : raw(scene) {}
    RenderSceneHandle(const RenderSceneHandle&) = delete;
    RenderSceneHandle& operator=(const RenderSceneHandle&) = delete;
    RenderSceneHandle(RenderSceneHandle&&) = delete;
    RenderSceneHandle& operator=(RenderSceneHandle&&) = delete;
    ~RenderSceneHandle() { mradm_scene_destroy(raw); }
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes): private adapter owning one Rust handle.
    MradmSceneHandle* raw;
};
} // namespace mradm::detail
