#include "live_binaural_renderer.h"

#include <utility>

#include "hrtf_state.h"
#include "live_rust_renderer.h"

namespace mradm::live_scene {

// The HRTF state is Rust mradm-render `binaural_hrtf` (PCM consistency 4.7 A); generations, frame
// staging, commands, warnings and the `.30-binaural` checkpoints are `live_binaural` (4.7 D).
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_binaural_renderer(const RendererConfig& config,
                                                                          DiagnosticSink diagnostics) {
    auto state = binaural_internal::HrtfState::prepare_live(config.sofa_path, config.sample_rate);
    if (!state) {
        return tl::unexpected{state.error()};
    }
    uint32_t spread = 0U;
    if (config.binaural_spread_mode == BinauralSpreadMode::none) {
        spread = 1U;
    } else if (config.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
        spread = 2U;
    }
    const MradmRenderLiveBinauralRequest request{config.sample_rate, spread, config.object_smoothing_frames};
    MradmRenderLive* raw = nullptr;
    RustError error;
    if (const auto code = mradm_render_live_binaural_create(
            (*state)->handle.get(), &request, &raw, error.message(), error.message_len());
        code != 0) {
        return error.error(code);
    }
    return adopt_live_renderer(raw, config.sample_rate, std::move(diagnostics), ".30-binaural");
}

} // namespace mradm::live_scene
