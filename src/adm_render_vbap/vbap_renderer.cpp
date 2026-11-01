#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_vbap.h"

#include "capability_records.h"
#include "render_common.h"
#include "scene_handle.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

using render_common::text;

class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const render_common::RustPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "saf-vbap: open_stream received an incompatible prepared state", {});
        }
        return render_common::open_file_stream(*prepared, plan);
    }
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> VbapRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    const std::string layout_id = plan.output_layout;
    const auto* layout = render_layouts::find_speaker_layout(layout_id, plan.speaker_geometry);
    if (layout == nullptr) {
        const std::string_view geometry = plan.speaker_geometry == SpeakerGeometry::apple ? "apple" : "standard";
        return make_error(
            ErrorCode::unsupported,
            fmt::format("unsupported VBAP output layout '{}' for speaker geometry '{}'", layout_id, geometry),
            {});
    }
    logs.log(
        LogLevel::info,
        "saf-vbap",
        fmt::format("speaker geometry: {}", plan.speaker_geometry == SpeakerGeometry::apple ? "apple" : "standard"));

    const auto routing_mode = plan.direct_speakers_routing_mode == DirectSpeakersRoutingMode::automatic
                                  ? DirectSpeakersRoutingMode::label
                                  : plan.direct_speakers_routing_mode;
    std::string routing_description{render_common::direct_speakers_routing_mode_name(routing_mode)};
    logs.log(LogLevel::info, "saf-vbap", fmt::format("DirectSpeakers routing: {}", routing_description));
    if (plan.scene_handle == nullptr) {
        return make_error(ErrorCode::internal_error, "saf-vbap requires the Rust render scene");
    }

    std::vector<MradmRenderSpeaker> speakers;
    std::ranges::transform(layout->speakers, std::back_inserter(speakers), [](const render_layouts::SpeakerSpec& s) {
        return MradmRenderSpeaker{text(s.label), s.azimuth, s.elevation, s.is_lfe ? 1U : 0U};
    });
    static const DirectSpeakersMatrix no_matrix;
    const auto* matrix = plan.direct_speakers_matrix.get();
    const render_common::MatrixRecords routes(matrix != nullptr ? *matrix : no_matrix); // borrows the labels
    const MradmRenderVbapRequest request{speakers.data(),
                                         speakers.size(),
                                         text(layout_id),
                                         text(plan.input_path),
                                         plan.speaker_spread_mode != SpeakerSpreadMode::none ? 1U : 0U,
                                         (routing_mode == DirectSpeakersRoutingMode::position ? 1U : 0U) |
                                             (routing_mode == DirectSpeakersRoutingMode::matrix ? 2U : 0U),
                                         matrix != nullptr ? 1U : 0U,
                                         routes.routes.data(),
                                         routes.routes.size(),
                                         plan.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U};
    MradmRenderPrepared* raw = nullptr;
    MradmRenderPreparedView view{};
    const auto code = mradm_render_vbap_prepare(plan.scene_handle->raw, &request, &raw, &view);
    auto prepared =
        render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::speaker, logs, "saf-vbap");
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    return std::static_pointer_cast<IPreparedRender>(*prepared);
}

Result<RenderMetrics> VbapRenderer::render_window(const IPreparedRender& prep,
                                                  const RenderPlan& plan,
                                                  ProgressSink& progress,
                                                  LogSink& logs) {
    const auto* prepared = dynamic_cast<const render_common::RustPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error, "saf-vbap: incompatible prepared state");
    }
    return render_common::render_speaker_pcm(plan, prepared->mix, prepared->outputs, "saf-vbap", progress, logs);
}

} // namespace

CapabilityReport vbap_capabilities() {
    return render_common::engine_capabilities(render_common::EngineBackend::saf);
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
