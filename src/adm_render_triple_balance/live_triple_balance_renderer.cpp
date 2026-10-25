#include "live_triple_balance_renderer.h"

#include <utility>

#include "live_rust_renderer.h"
#include "speaker_pcm.h"

namespace mradm::live_scene {

// The renderer is Rust mradm-render `live_triple_balance` (PCM consistency 4.6).
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_triple_balance_renderer(const RendererConfig& config,
                                                                                DiagnosticSink diagnostics) {
    uint32_t spread = 0U;
    if (config.speaker_spread_mode == SpeakerSpreadMode::none) {
        spread = 1U;
    } else if (config.speaker_spread_mode == SpeakerSpreadMode::mdap) {
        spread = 2U;
    }
    const MradmRenderLiveTripleBalanceRequest request{render_common::text(config.output_layout),
                                                      config.speaker_geometry == SpeakerGeometry::standard ? 0U : 1U,
                                                      spread,
                                                      config.sofa_path.empty() ? 0U : 1U,
                                                      config.sample_rate,
                                                      config.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                                      config.object_smoothing_frames};
    MradmRenderLive* raw = nullptr;
    RustError error(config.output_layout.size());
    const auto code = mradm_render_live_triple_balance_create(&request, &raw, error.message(), error.message_len());
    if (code != 0) {
        return error.error(code);
    }
    return adopt_live_renderer(raw, config.sample_rate, std::move(diagnostics), ".30-triple");
}

} // namespace mradm::live_scene
