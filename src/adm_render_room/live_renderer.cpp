#include "live_renderer.h"

#include <string>
#include <utility>

#include "live_rust_renderer.h"
#include "room_internal.h"

namespace mradm::live_scene {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
MradmAdmText text(const std::string& value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}
} // namespace

// The selection, the checks and the renderers are Rust mradm-engine `scene::renderer` (PCM
// consistency 5.4 A); this shell converts the configuration and adopts the handle. It lives with
// the room renderer for the room profile and the room control.
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_renderer(const RendererConfig& config,
                                                                 DiagnosticSink diagnostics) {
    const auto sofa = utf8(config.sofa_path);
    const auto sofa_name = utf8(config.sofa_path.filename());
    const auto hrir = utf8(config.room.hrir_manifest);
    const auto reference = config.screens.reference.value_or(ScreenGeometry{});
    const MradmEngineSceneRendererRequest request{
        static_cast<uint32_t>(config.renderer),
        text(config.output_layout),
        config.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
        static_cast<uint32_t>(config.speaker_spread_mode),
        static_cast<uint32_t>(config.binaural_spread_mode),
        config.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
        config.object_smoothing_frames,
        config.sample_rate,
        config.sofa_path.empty() ? 0U : 1U,
        text(sofa),
        text(sofa_name),
        config.screens.reference ? 1U : 0U,
        room_internal::geometry(reference),
        config.screens.reproduction ? 1U : 0U,
        room_internal::geometry(config.screens.reproduction.value_or(reference)),
        config.room.profile ? room_internal::Access::get(*config.room.profile) : nullptr,
        text(hrir),
        config.room.metres_per_unit,
        static_cast<uint32_t>(config.room.quality),
        diagnostics ? 1U : 0U};
    MradmRenderLive* raw = nullptr;
    uint32_t sample_rate = 0;
    MradmAdmText suffix{};
    RustError error(config.output_layout.size() + sofa.size() + hrir.size(), sofa.size() + hrir.size());
    if (const auto code = mradm_engine_scene_renderer_create(&request,
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
