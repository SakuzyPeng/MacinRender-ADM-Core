#include "live_room_renderer.h"

#include "live_rust_renderer.h"
#include "room_internal.h"

namespace mradm::live_scene {
// Generations, label positions, the screenRef warning, the room runtime and end of input are Rust
// mradm-render `room_stream::Live` (PCM consistency 4.6 E); this shell prepares the room set.
Result<std::unique_ptr<ILiveSceneRenderer>> create_live_room_renderer(const RendererConfig& config,
                                                                      DiagnosticSink diagnostics) {
    auto screen_valid = validate_screen_config(config.screens);
    if (!screen_valid) {
        return tl::unexpected{screen_valid.error()};
    }
    auto prepared = room_internal::prepare(config.room, config.sofa_path);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    if (config.sample_rate != (*prepared)->info.sample_rate || config.output_layout != "binaural") {
        return make_error(ErrorCode::unsupported, "room-binaural 需要与房间包采样率一致的双耳输出");
    }
    if (config.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
        return make_error(ErrorCode::unsupported, "room-binaural 不支持 SAF covariance spreader");
    }
    const room_internal::ConfigRecord record(config.room);
    const auto screens = room_internal::screens(config.screens, config.screens.reference.value_or(ScreenGeometry{}));
    const auto spread = config.binaural_spread_mode != BinauralSpreadMode::none ? 1U : 0U;
    MradmRenderLive* raw = nullptr;
    RustError error;
    if (const auto code = mradm_render_live_room_create((*prepared)->handle.get(),
                                                        &record.record,
                                                        spread,
                                                        &screens,
                                                        diagnostics ? 1U : 0U,
                                                        &raw,
                                                        error.message(),
                                                        error.message_len(),
                                                        error.context(),
                                                        error.context_len());
        code != 0) {
        return error.error(code);
    }
    auto control = room_internal::control(mradm_render_live_room_control(raw));
    return adopt_live_renderer(raw, (*prepared)->info.sample_rate, std::move(diagnostics), "", std::move(control));
}
} // namespace mradm::live_scene
