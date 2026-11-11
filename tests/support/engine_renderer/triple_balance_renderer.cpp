#include <memory>

#include "adm/render_triple_balance.h"

#include "capability_records.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {

using render_common::EngineBackend;

// The compatibility copy of the scene, its report, the input checks and the numeric plan are
// prepared in Rust (mradm-engine `backend`, mradm-render `triple_balance`, PCM consistency 4.6)
// from the Rust render scene; offline windows, with the size and D-mode processors, render in the
// engine and file streaming in mradm-engine `stream` (mradm-render `triple_balance_stream`).
class TripleBalanceRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return triple_balance_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = render_common::prepare_backend(EngineBackend::triple_balance, plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& state, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        if (plan.triple_balance_mode == TripleBalanceMode::d) {
            return make_error(ErrorCode::unsupported,
                              "Triple Balance D mode currently supports offline rendering only");
        }
        const auto* prepared = dynamic_cast<const render_common::EnginePrepared*>(&state);
        if (prepared == nullptr) {
            return make_error(ErrorCode::internal_error, "triple-balance: incompatible prepared state");
        }
        return render_common::open_engine_stream(*prepared, plan);
    }
};

} // namespace

CapabilityReport triple_balance_capabilities() {
    return render_common::engine_capabilities(EngineBackend::triple_balance);
}

std::unique_ptr<IRenderer> create_triple_balance_renderer() {
    return std::make_unique<TripleBalanceRenderer>();
}

} // namespace mradm
