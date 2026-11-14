#include <memory>

#include "adm/render.h"
#include "adm/render_binaural.h"

#include "capability_records.h"
#include "speaker_pcm.h"

namespace mradm {

// The HRTF state (4.7 A), the source plan, OLA convolution and the offline loop (4.7 B) and the
// file stream (4.7 C) are Rust mradm-render `binaural_hrtf`, `binaural`, `binaural_render` and
// `binaural_stream`; the preparation and offline windows run in mradm-engine (5.2 C1), the file
// stream opens in mradm-engine `stream` (5.3 A).
namespace binaural_internal {
namespace {

using render_common::EnginePrepared;

class BinauralRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return binaural_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = render_common::prepare_backend(render_common::EngineBackend::binaural, plan, logs);
        if (!prepared) {
            return mradm::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "binaural: open_stream received an incompatible prepared state", {});
        }
        (void) logs;
        return render_common::open_engine_stream(*prepared, plan);
    }
};

} // namespace
} // namespace binaural_internal

CapabilityReport binaural_capabilities() {
    return render_common::engine_capabilities(render_common::EngineBackend::binaural);
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<binaural_internal::BinauralRenderer>();
}

bool binaural_sofa_supported() {
#if MR_ADM_ENABLE_SOFA
    return true;
#else
    return false;
#endif
}

} // namespace mradm
