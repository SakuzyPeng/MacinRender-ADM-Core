#include <memory>

#include "adm/render.h"
#include "adm/render_hoa.h"

#include "capability_records.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

using render_common::EngineBackend;
using render_common::EnginePrepared;

// The encoder plan and its live-control bindings are prepared in Rust (mradm-engine `backend`,
// mradm-render `hoa`); offline windows render in the engine and file streaming in mradm-engine
// `stream`, which checks the input format the plan was prepared for.
class HoaRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return hoa_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = render_common::prepare_backend(EngineBackend::hoa, plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    // Live gain pre-scales the matching input channels before the (linear) encode, so it scales
    // an object's direct and diffuse parts alike; a DirectSpeakers override with a speaker label
    // targets that bed channel and wins over the whole-object override.
    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "hoa-encode: open_stream received an incompatible prepared state", {});
        }
        return render_common::open_engine_stream(*prepared, plan);
    }
};

} // namespace

CapabilityReport hoa_capabilities() {
    return render_common::engine_capabilities(EngineBackend::hoa);
}

std::unique_ptr<IRenderer> create_hoa_renderer() {
    return std::make_unique<HoaRenderer>();
}

} // namespace mradm
