#include <memory>
#include <string>

#include "adm/render_triple_balance.h"

#include "render_common.h"
#include "render_state.h"
#include "scene_handle.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {
using render_common::text;

[[nodiscard]] std::string string_of(MradmAdmText t) {
    return {reinterpret_cast<const char*>(t.data), t.len};
}

using TripleBalancePrepared = triple_balance::Prepared;

// The compatibility copy of the scene, its report, the input checks and the numeric plan are
// built in Rust mradm-render `triple_balance` (PCM consistency 4.6) from the Rust render scene;
// offline rendering, with the size and D-mode processors, runs in its `offline` driver and file
// streaming in its `triple_balance_stream`.
class TripleBalanceRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return triple_balance_capabilities(); }
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& state, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        if (plan.triple_balance_mode == TripleBalanceMode::d) {
            return make_error(ErrorCode::unsupported,
                              "Triple Balance D mode currently supports offline rendering only");
        }
        const auto* prepared = dynamic_cast<const TripleBalancePrepared*>(&state);
        if (prepared == nullptr) {
            return make_error(ErrorCode::internal_error, "triple-balance: incompatible prepared state");
        }
        if (prepared->mode != TripleBalanceMode::standard) {
            return make_error(ErrorCode::unsupported,
                              "Triple Balance D mode currently supports offline rendering only");
        }
        return triple_balance::open_stream(*prepared, plan);
    }
    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& state, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

Result<std::shared_ptr<IPreparedRender>> TripleBalanceRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    if (plan.scene_handle == nullptr) {
        return make_error(ErrorCode::internal_error, "triple-balance requires the Rust render scene");
    }
    const MradmRenderTripleBalanceRequest request{text(plan.output_layout),
                                                  plan.triple_balance_mode == TripleBalanceMode::d ? 1U : 0U,
                                                  static_cast<uint32_t>(plan.speaker_spread_mode),
                                                  plan.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                                  plan.speaker_geometry == SpeakerGeometry::standard ? 1U : 0U,
                                                  plan.object_smoothing_frames,
                                                  static_cast<uint32_t>(plan.direct_speakers_routing_mode),
                                                  plan.direct_speakers_matrix ? 1U : 0U,
                                                  text(plan.input_path)};
    MradmRenderPrepared* raw = nullptr;
    MradmRenderPreparedView view{};
    MradmRenderTripleBalanceView report{};
    const auto code = mradm_render_triple_balance_prepare(plan.scene_handle->raw, &request, &raw, &view, &report);
    // The backend report also describes a rejected preparation.
    if (raw != nullptr && plan.renderer_semantics_sink) {
        plan.renderer_semantics_sink(string_of(report.report));
    }
    auto rust =
        render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::speaker, logs, "triple-balance");
    if (!rust) {
        return tl::unexpected{rust.error()};
    }
    auto prepared = std::make_shared<TripleBalancePrepared>();
    prepared->output_channels = (*rust)->outputs;
    prepared->sample_rate = plan.scene.info.sample_rate;
    prepared->mode = plan.triple_balance_mode;
    prepared->rust = std::move(*rust);
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

Result<RenderMetrics> TripleBalanceRenderer::render_window(const IPreparedRender& state,
                                                           const RenderPlan& plan,
                                                           ProgressSink& progress,
                                                           LogSink& logs) {
    const auto* prepared = dynamic_cast<const TripleBalancePrepared*>(&state);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error, "triple-balance: incompatible prepared state");
    }
    if (prepared->mode != plan.triple_balance_mode) {
        return make_error(ErrorCode::invalid_argument, "Triple Balance prepared mode does not match render options");
    }
    if (prepared->mode == TripleBalanceMode::d) {
        const auto* layout = render_layouts::find_speaker_layout(plan.output_layout);
        if (layout == nullptr || layout->speakers.size() != prepared->output_channels ||
            plan.scene.info.sample_rate != prepared->sample_rate) {
            return make_error(ErrorCode::invalid_argument,
                              "D mode prepared layout or sample rate does not match render options");
        }
    }
    return render_common::render_speaker_pcm(
        plan, prepared->rust->mix, prepared->output_channels, "triple-balance", progress, logs);
}

} // namespace

CapabilityReport triple_balance_capabilities() {
    CapabilityReport result;
    result.backend_name = "triple-balance";
    result.backend_version = "1";
    result.supports_objects = true;
    result.supports_direct_speakers = true;
    result.supports_render_window = true;
    result.modes = {{"standard", "Standard", {"4+7+0", "9.1.6", "9+10+3"}, {}, true},
                    {"d", "D mode", {"9.1.6", "9+10+3"}, {48000}, false}};
    // Fixed room layouts; no custom SAF layouts or listener-oriented geometry.
    result.supported_layouts = {
        {"4+7+0", "7.1.4", 12, true, 1, true, false},
        {"9.1.6", "9.1.6", 16, true, 1, true, false},
        {"9+10+3", "22.2 (experimental)", 24, true, 2, true, false},
    };
    return result;
}

std::unique_ptr<IRenderer> create_triple_balance_renderer() {
    return std::make_unique<TripleBalanceRenderer>();
}

} // namespace mradm
