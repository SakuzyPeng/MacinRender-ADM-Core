#include <memory>

#include "adm/render.h"
#include "adm/render_vbap.h"

namespace mradm {

namespace {

class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<void> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

Result<void> VbapRenderer::render(const RenderPlan& /*plan*/, ProgressSink& /*progress*/, LogSink& /*logs*/) {
    return make_error(ErrorCode::unsupported, "VBAP rendering is not yet implemented", "backend=saf-vbap");
}

} // namespace

CapabilityReport vbap_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-vbap";
    r.backend_version = "0.0.0";
    r.supports_objects = true;
    r.supports_direct_speakers = false;
    r.supports_hoa = false;
    r.supported_layouts = {
        {"0+2+0", "Stereo"},
        {"0+5+0", "5.0"},
        {"0+7+0", "7.0"},
        {"4+5+0", "5.1.4"},
        {"4+7+0", "7.1.4"},
        {"9+10+3", "9.1.6"},
    };
    return r;
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
