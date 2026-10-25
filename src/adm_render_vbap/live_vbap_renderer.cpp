#include "live_vbap_renderer.h"

#include <string>
#include <utility>

#include "live_rust_renderer.h"
#include "speaker_pcm.h"

namespace mradm::live_scene {

// The renderer is Rust mradm-render `live_vbap` (PCM consistency 4.5).
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_vbap_renderer(const RendererConfig& config,
                                                                      DiagnosticSink diagnostics) {
    const MradmRenderLiveVbapRequest request{render_common::text(config.output_layout),
                                             config.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
                                             config.speaker_spread_mode != SpeakerSpreadMode::none ? 1U : 0U,
                                             config.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                             config.object_smoothing_frames};
    MradmRenderLive* raw = nullptr;
    RustError error(config.output_layout.size());
    const auto code = mradm_render_live_vbap_create(&request, &raw, error.message(), error.message_len());
    if (code != 0) {
        return error.error(code);
    }
    auto suffix = ".30-vbap" + std::to_string(mradm_render_live_channels(raw));
    return adopt_live_renderer(raw, config.sample_rate, std::move(diagnostics), std::move(suffix));
}

} // namespace mradm::live_scene
