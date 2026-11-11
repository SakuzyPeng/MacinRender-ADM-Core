#include "live_renderer.h"

#include <string>
#include <utility>

#include "live_renderer_adapter.h"
#include "room_internal.h"
#include "scene_renderer_request.h"

namespace mradm::live_scene {

// The selection, the checks and the renderers are Rust mradm-engine `scene::renderer` (PCM
// consistency 5.4 A); this test shell adopts the handle behind the C++ renderer interface.
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_renderer(const RendererConfig& config,
                                                                 DiagnosticSink diagnostics) {
    const SceneRendererRequest request(config, static_cast<bool>(diagnostics));
    MradmRenderLive* raw = nullptr;
    uint32_t sample_rate = 0;
    MradmAdmText suffix{};
    auto error = request.error();
    if (const auto code = mradm_engine_scene_renderer_create(request.get(),
                                                             &raw,
                                                             &sample_rate,
                                                             &suffix,
                                                             error.message(),
                                                             error.message_len(),
                                                             error.context(),
                                                             error.context_len());
        code != 0) {
        return error.error(code);
    }
    std::string trace{reinterpret_cast<const char*>(suffix.data), suffix.len};
    auto control = room_internal::control(mradm_render_live_room_control(raw));
    return adopt_live_renderer(raw, sample_rate, std::move(diagnostics), std::move(trace), std::move(control));
}

} // namespace mradm::live_scene
