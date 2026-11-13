#include "scene_renderer_request.h"

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

SceneRendererRequest::SceneRendererRequest(const RendererConfig& config, bool sink)
    : sofa_(utf8(config.sofa_path)), sofa_name_(utf8(config.sofa_path.filename())),
      hrir_(utf8(config.room.hrir_manifest)), layout_(config.output_layout.size()) {
    const auto reference = config.screens.reference.value_or(ScreenGeometry{});
    request_ = {static_cast<uint32_t>(config.renderer),
                text(config.output_layout),
                config.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
                static_cast<uint32_t>(config.speaker_spread_mode),
                static_cast<uint32_t>(config.binaural_spread_mode),
                config.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                config.object_smoothing_frames,
                config.sample_rate,
                config.sofa_path.empty() ? 0U : 1U,
                text(sofa_),
                text(sofa_name_),
                config.screens.reference ? 1U : 0U,
                room_internal::geometry(reference),
                config.screens.reproduction ? 1U : 0U,
                room_internal::geometry(config.screens.reproduction.value_or(reference)),
                config.room.profile ? room_internal::Access::get(*config.room.profile) : nullptr,
                text(hrir_),
                config.room.metres_per_unit,
                static_cast<uint32_t>(config.room.quality),
                sink ? 1U : 0U};
}

} // namespace mradm::live_scene
