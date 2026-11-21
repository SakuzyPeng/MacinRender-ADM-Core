#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "adm/render.h"

#include "../adm_core/rust_services_ffi.h"

namespace mradm::detail {
// The imported Rust render scene (mradm-scene) behind RenderPlan::scene_handle; every backend,
// the Apple island included, reads it.
struct RenderSceneHandle {
    explicit RenderSceneHandle(MradmSceneHandle* scene) : raw(scene) {}
    RenderSceneHandle(const RenderSceneHandle&) = delete;
    RenderSceneHandle& operator=(const RenderSceneHandle&) = delete;
    RenderSceneHandle(RenderSceneHandle&&) = delete;
    RenderSceneHandle& operator=(RenderSceneHandle&&) = delete;
    ~RenderSceneHandle() { mradm::rust().scene_destroy(raw); }
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes): private adapter owning one Rust handle.
    MradmSceneHandle* raw;
};
} // namespace mradm::detail
