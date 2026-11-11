#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "adm/render.h"

#include "../adm_metadata/scene_ffi.h"

namespace mradm::detail {
// The imported Rust render scene (mradm-scene) behind RenderPlan::scene_handle; every backend,
// the Apple island included, reads it.
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

// The audio format and source of the scene; nullopt on a rejected view.
[[nodiscard]] inline std::optional<MradmSceneImportInfo> scene_info(const RenderSceneHandle& scene) {
    MradmSceneImportInfo info{};
    MradmAdmView view{};
    std::array<uint8_t, 256> message{};
    if (mradm_scene_view(scene.raw, &info, &view, message.data(), message.size()) != 0) {
        return std::nullopt;
    }
    return info;
}
} // namespace mradm::detail
