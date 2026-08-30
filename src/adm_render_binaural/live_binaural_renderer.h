#pragma once

#include <memory>

#include "live_scene_renderer.h"

namespace mradm::live_scene {

[[nodiscard]] Result<std::unique_ptr<ILiveSceneRenderer>> create_live_binaural_renderer(const RendererConfig& config,
                                                                                        DiagnosticSink diagnostics);

} // namespace mradm::live_scene
