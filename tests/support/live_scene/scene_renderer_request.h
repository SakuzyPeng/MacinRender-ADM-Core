#pragma once

#include <cstddef>
#include <string>

#include "ffi_error.h"
#include "live_scene_renderer.h"
#include "routing_test_ffi.h"

namespace mradm::live_scene {

// The Rust request of a Scene renderer configuration (mradm-engine `scene::renderer`)
// with the UTF-8 texts it points at; it also borrows `config`. It lives with the
// room renderer for the room profile handle.
class SceneRendererRequest {
  public:
    SceneRendererRequest(const RendererConfig& config, bool sink);
    SceneRendererRequest(const SceneRendererRequest&) = delete;
    SceneRendererRequest& operator=(const SceneRendererRequest&) = delete;
    SceneRendererRequest(SceneRendererRequest&&) = delete;
    SceneRendererRequest& operator=(SceneRendererRequest&&) = delete;
    ~SceneRendererRequest() = default;

    [[nodiscard]] const MradmEngineSceneRendererRequest* get() const noexcept { return &request_; }
    // A buffer for the creation's errors, which can quote the layout and the paths.
    [[nodiscard]] FfiError error() const {
        return FfiError(layout_ + sofa_.size() + hrir_.size(), sofa_.size() + hrir_.size());
    }

  private:
    std::string sofa_;
    // The constructor uses this storage to keep the FFI file-name view alive.
    // cppcheck-suppress unusedStructMember
    std::string sofa_name_;
    std::string hrir_;
    std::size_t layout_{0};
    MradmEngineSceneRendererRequest request_{};
};

} // namespace mradm::live_scene
