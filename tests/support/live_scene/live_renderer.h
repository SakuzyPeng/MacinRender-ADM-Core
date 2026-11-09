#pragma once
#include "live_scene_interface.h"

namespace mradm::live_scene {
// Test support (PCM consistency 5.4 B): one Scene stream renderer outside the stream engine.
// The Scene stream renderer of config.renderer: SAF VBAP, SAF binaural, room-binaural or Triple Balance.
[[nodiscard]] Result<std::unique_ptr<ILiveSceneRenderer>> create_live_renderer(const RendererConfig& config,
                                                                               DiagnosticSink diagnostics);
} // namespace mradm::live_scene
