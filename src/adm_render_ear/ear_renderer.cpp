#include <memory>

#include "adm/render.h"
#include "adm/render_ear.h"

#include "capability_records.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

using render_common::EngineBackend;
using render_common::EnginePrepared;

// The EAR layout, gain plan, decorrelation filters and live-control bindings are prepared in Rust
// (mradm-engine `backend`, mradm-render `ear`); offline windows render in the engine and file
// streaming in mradm-render `stream`.
class EarRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return ear_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = render_common::prepare_backend(EngineBackend::ear, plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    // Live gain pre-scales the matching input channels before the (linear) gain mix and
    // decorrelation; a DirectSpeakers override with a speaker label targets that bed channel.
    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "ear: open_stream received an incompatible prepared state", {});
        }
        return render_common::open_file_stream(*prepared, plan);
    }
};

} // namespace

CapabilityReport ear_capabilities() {
    return render_common::engine_capabilities(EngineBackend::ear);
}

std::unique_ptr<IRenderer> create_ear_renderer() {
    return std::make_unique<EarRenderer>();
}

} // namespace mradm
