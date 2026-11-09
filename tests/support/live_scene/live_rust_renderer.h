#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "live_scene_interface.h"
#include "routing_ffi.h"
#include "scene_renderer_request.h"

namespace mradm::live_scene {

// The ILiveSceneRenderer of a Rust mradm-render Scene stream renderer, which owns generations,
// frame staging, warnings and the checkpoints named `<scene renderer key><trace_suffix>-*`. A room
// renderer also hands over its room control.
[[nodiscard]] std::unique_ptr<ILiveSceneRenderer> adopt_live_renderer(MradmRenderLive* handle,
                                                                      uint32_t sample_rate,
                                                                      DiagnosticSink diagnostics,
                                                                      std::string trace_suffix,
                                                                      std::shared_ptr<IRoomControl> room = {});

} // namespace mradm::live_scene
