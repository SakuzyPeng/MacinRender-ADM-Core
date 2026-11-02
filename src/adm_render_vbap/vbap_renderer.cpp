#include <memory>

#include "adm/render.h"
#include "adm/render_vbap.h"

#include "capability_records.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

using render_common::EngineBackend;
using render_common::EnginePrepared;

// The layout lookup, routing, gain plan and live-control bindings are prepared in Rust
// (mradm-engine `backend`, mradm-render `vbap`); offline windows render in the engine and file
// streaming in mradm-render `stream`.
class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return vbap_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = render_common::prepare_backend(EngineBackend::saf, plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "saf-vbap: open_stream received an incompatible prepared state", {});
        }
        return render_common::open_file_stream(*prepared, plan);
    }
};

} // namespace

CapabilityReport vbap_capabilities() {
    return render_common::engine_capabilities(EngineBackend::saf);
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
