#include <memory>
#include <string_view>

#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_hoa.h"

#include "render_common.h"
#include "scene_handle.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

constexpr uint16_t k_hoa3_channels = 16;

using render_common::RustPrepared;

// The encoder plan and its live-control bindings are built in Rust mradm-render `hoa` (PCM
// consistency 4.5) from the Rust render scene; offline rendering and file streaming run in its
// `offline` and `stream`. The prepared plan keeps the input format it was prepared for.
[[nodiscard]] Result<const RustPrepared*>
matching(const IPreparedRender& prep, const RenderPlan& plan, std::string_view stage) {
    const auto* prepared = dynamic_cast<const RustPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error,
                          fmt::format("hoa-encode: {} received an incompatible prepared state", stage),
                          {});
    }
    if (plan.scene.info.num_channels != prepared->inputs || plan.scene.info.sample_rate != prepared->sample_rate) {
        return make_error(ErrorCode::invalid_argument, "HOA prepared/input format mismatch");
    }
    return prepared;
}

class HoaRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return hoa_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic) {
            return make_error(ErrorCode::unsupported,
                              "HOA renderer does not support explicit DirectSpeakers routing; use automatic",
                              {});
        }
        if (plan.scene_handle == nullptr) {
            return make_error(ErrorCode::internal_error, "hoa-encode requires the Rust render scene");
        }
        const MradmRenderHoaRequest request{render_common::text(plan.output_layout),
                                            plan.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U};
        MradmRenderPrepared* raw = nullptr;
        MradmRenderPreparedView view{};
        const auto code = mradm_render_hoa_prepare(plan.scene_handle->raw, &request, &raw, &view);
        auto prepared = render_common::adopt_prepared(code, raw, view, 1U, logs, "hoa-encode");
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        (*prepared)->outputs = k_hoa3_channels;
        (*prepared)->inputs = plan.scene.info.num_channels;
        (*prepared)->sample_rate = plan.scene.info.sample_rate;
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& prep, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override {
        const auto prepared = matching(prep, plan, "render_window");
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return render_common::render_speaker_pcm(plan, (*prepared)->mix, k_hoa3_channels, "hoa-encode", progress, logs);
    }

    // Live gain pre-scales the matching input channels before the (linear) encode, so it scales
    // an object's direct and diffuse parts alike; a DirectSpeakers override with a speaker label
    // targets that bed channel and wins over the whole-object override.
    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto prepared = matching(prep, plan, "open_stream");
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return render_common::open_file_stream(**prepared, plan);
    }
};

} // namespace

CapabilityReport hoa_capabilities() {
    CapabilityReport r;
    r.backend_name = "hoa-encode";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    r.supports_render_window = true; // block-aligned seek + 1 pre-roll block (diffuse delay line)
    r.supported_layouts = {
        {"hoa3", "HOA 3rd Order (16ch, ACN/SN3D)", 16, true, 0, false},
    };
    return r;
}

std::unique_ptr<IRenderer> create_hoa_renderer() {
    return std::make_unique<HoaRenderer>();
}

} // namespace mradm
