#pragma once
#include "live_scene_renderer.h"

namespace mradm::live_scene {
// The Scene stream renderer of config.renderer: SAF VBAP, SAF binaural, room-binaural or Triple Balance.
[[nodiscard]] Result<std::unique_ptr<ILiveSceneRenderer>> create_live_renderer(const RendererConfig& config,
                                                                               DiagnosticSink diagnostics);
} // namespace mradm::live_scene
