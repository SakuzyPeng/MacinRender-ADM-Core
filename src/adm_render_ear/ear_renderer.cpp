#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "adm/render.h"
#include "adm/render_ear.h"

#include "render_common.h"
#include "scene_handle.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

using render_common::RustPrepared;
using render_common::text;

// The EAR layout, gain plan, decorrelation filters and live-control bindings are built in Rust
// mradm-render `ear` (PCM consistency 4.5) from the Rust render scene; offline rendering and file
// streaming run in its `offline` and `stream`.
class EarRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return ear_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        const bool matrix_routing = plan.direct_speakers_routing_mode == DirectSpeakersRoutingMode::matrix;
        if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic && !matrix_routing) {
            return make_error(
                ErrorCode::unsupported, "EAR renderer supports only automatic or matrix DirectSpeakers routing", {});
        }
        if (plan.scene_handle == nullptr) {
            return make_error(ErrorCode::internal_error, "ear requires the Rust render scene");
        }
        static const DirectSpeakersMatrix no_matrix;
        const auto* matrix = plan.direct_speakers_matrix.get();
        const render_common::MatrixRecords routes(matrix != nullptr ? *matrix : no_matrix); // borrows the labels
        const MradmRenderEarRequest request{text(plan.output_layout),
                                            plan.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
                                            text(plan.input_path),
                                            matrix_routing ? 1U : 0U,
                                            matrix != nullptr ? 1U : 0U,
                                            routes.routes.data(),
                                            routes.routes.size(),
                                            plan.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U};
        MradmRenderPrepared* raw = nullptr;
        MradmRenderPreparedView view{};
        const auto code = mradm_render_ear_prepare(plan.scene_handle->raw, &request, &raw, &view);
        auto prepared = render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::ear, logs, "ear");
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& prep, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const RustPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "ear: render_window received an incompatible prepared state", {});
        }
        return render_common::render_speaker_pcm(plan, prepared->mix, prepared->outputs, "ear", progress, logs);
    }

    // Live gain pre-scales the matching input channels before the (linear) gain mix and
    // decorrelation; a DirectSpeakers override with a speaker label targets that bed channel.
    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const RustPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "ear: open_stream received an incompatible prepared state", {});
        }
        return render_common::open_file_stream(*prepared, plan);
    }
};

} // namespace

CapabilityReport ear_capabilities() {
    CapabilityReport r;
    r.backend_name = "libear";
    r.backend_version = "rust-ear-0.1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = true; // HOA block decode via GainCalculatorHOA
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_screen_ref = false;
    r.supports_diffuse = true;
    r.supports_render_window = true; // seek + 1-block pre-roll; bit-exact windowed output
    for (const auto& spec : render_layouts::speaker_layouts()) {
        CapabilityReport::Layout layout;
        layout.id = std::string{spec.id};
        layout.display_name = std::string{spec.display_name};
        layout.channel_count = static_cast<uint16_t>(spec.speakers.size());
        layout.lfe_count =
            static_cast<uint16_t>(std::ranges::count_if(spec.speakers, [](const auto& s) { return s.is_lfe; }));
        layout.is_3d = std::ranges::any_of(spec.speakers,
                                           [](const auto& s) { return !s.is_lfe && std::fabs(s.elevation) > 1.0e-6F; });
        layout.supports_spread = true;
        r.supported_layouts.push_back(std::move(layout));
    }
    return r;
}

std::unique_ptr<IRenderer> create_ear_renderer() {
    return std::make_unique<EarRenderer>();
}

} // namespace mradm
