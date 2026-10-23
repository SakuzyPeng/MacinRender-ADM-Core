#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_vbap.h"

#include "../adm_audio/wav_backend.h"
#include "render_common.h"
#include "scene_handle.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

namespace mradm {

namespace {

struct LayoutSpec {
    std::vector<VbapSpeakerSpec> speakers;
};

struct RegistryEntry {
    std::string id;
    std::string display_name;
    std::vector<VbapSpeakerSpec> speakers;
};

std::vector<RegistryEntry>& custom_layout_registry() {
    static std::vector<RegistryEntry> reg;
    return reg;
}

[[nodiscard]] VbapSpeakerSpec to_vbap_speaker(const render_layouts::SpeakerSpec& speaker) {
    return {speaker.azimuth, speaker.elevation, std::string{speaker.label}, speaker.is_lfe};
}

[[nodiscard]] LayoutSpec layout_spec_from_shared(const render_layouts::SpeakerLayout& layout) {
    LayoutSpec spec;
    spec.speakers.reserve(layout.speakers.size());
    std::ranges::transform(layout.speakers, std::back_inserter(spec.speakers), to_vbap_speaker);
    return spec;
}

[[nodiscard]] std::optional<LayoutSpec> layout_spec(std::string_view layout_id, SpeakerGeometry geometry) {
    if (const auto* shared = render_layouts::find_speaker_layout(layout_id, geometry); shared != nullptr) {
        return layout_spec_from_shared(*shared);
    }
    const auto& reg = custom_layout_registry();
    const auto it = std::ranges::find_if(reg, [layout_id](const RegistryEntry& e) { return e.id == layout_id; });
    if (it != reg.end()) {
        return LayoutSpec{it->speakers};
    }
    return std::nullopt;
}

using render_common::text;

// The gain plan and its live-control bindings are built in Rust mradm-render `vbap` (PCM
// consistency 4.5) from the Rust render scene; immutable and reused across render_window()
// calls (PreviewSession scrubbing).
struct VbapPrepared final : IPreparedRender {
    std::unique_ptr<MradmRenderVbap, decltype(&mradm_render_vbap_destroy)> handle{nullptr, &mradm_render_vbap_destroy};
    render_common::SpeakerMix mix;
    uint16_t outputs{0};
};

// Realtime streaming over the prepared plan in Rust mradm-render `stream`: the offline block size
// and mixer into a FIFO, so a gap-free run from frame 0 is bit-identical to render_window. Live
// gain pre-scales each input channel by its object's (or speaker label's) override.
class VbapStream final : public IRenderStream {
  public:
    VbapStream(MradmRenderStream* raw, const RenderPlan& plan, uint16_t outputs)
        : handle_(raw, &mradm_render_stream_destroy), input_path_(plan.input_path), outputs_(outputs),
          sample_rate_(plan.scene.info.sample_rate), output_layout_(plan.output_layout) {}

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        render_common::DriverError error;
        std::size_t produced = 0;
        if (const auto code = mradm_render_stream_process(handle_.get(),
                                                          out.data(),
                                                          out.size(),
                                                          frames,
                                                          &produced,
                                                          error.location(),
                                                          error.message(),
                                                          error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        render_common::DriverError error;
        if (const auto code =
                mradm_render_stream_seek(handle_.get(), frame, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return {};
    }

    // Topology scales (diffuse / extent / divergence) do not apply to VBAP and are ignored.
    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = render_common::override_records(overrides);
        if (mradm_render_stream_overrides(handle_.get(), records.data(), records.size()) != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] uint32_t out_channels() const override { return outputs_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return output_layout_; }

  private:
    std::unique_ptr<MradmRenderStream, decltype(&mradm_render_stream_destroy)> handle_;
    std::string input_path_;
    uint16_t outputs_;
    uint32_t sample_rate_;
    std::string output_layout_;
};

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
        const auto* prepared = dynamic_cast<const VbapPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "saf-vbap: open_stream received an incompatible prepared state", {});
        }
        const auto path = audio::wav_path_utf8(plan.input_path);
        render_common::DriverError error;
        MradmRenderStream* raw = nullptr;
        if (const auto code = mradm_render_vbap_stream_open(prepared->handle.get(),
                                                            reinterpret_cast<const uint8_t*>(path.data()),
                                                            path.size(),
                                                            plan.default_interp_ms,
                                                            plan.object_smoothing_frames,
                                                            &raw,
                                                            error.location(),
                                                            error.message(),
                                                            error.message_len());
            code != 0) {
            return error.error(code, plan.input_path);
        }
        return std::unique_ptr<IRenderStream>{std::make_unique<VbapStream>(raw, plan, prepared->outputs)};
    }
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> VbapRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    const std::string layout_id = plan.output_layout;
    auto layout = layout_spec(layout_id, plan.speaker_geometry);
    if (!layout.has_value()) {
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
    std::ranges::transform(layout->speakers, std::back_inserter(speakers), [](const VbapSpeakerSpec& s) {
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
    auto prepared = std::make_shared<VbapPrepared>();
    MradmRenderVbap* raw = nullptr;
    MradmRenderVbapView view{};
    const auto code = mradm_render_vbap_prepare(plan.scene_handle->raw, &request, &raw, &view);
    prepared->handle.reset(raw);
    if (raw == nullptr) {
        return make_error(static_cast<ErrorCode>(code), "saf-vbap preparation failed");
    }
    const auto string_of = [](MradmAdmText t) { return std::string{reinterpret_cast<const char*>(t.data), t.len}; };
    for (const auto& line : std::span{view.logs, view.logs_len}) {
        logs.log(static_cast<LogLevel>(line.level), "saf-vbap", string_of(line.message));
    }
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), string_of(view.message), string_of(view.context));
    }
    prepared->mix = {view.plan, view.tracks};
    prepared->outputs = static_cast<uint16_t>(layout->speakers.size());
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

Result<RenderMetrics> VbapRenderer::render_window(const IPreparedRender& prep,
                                                  const RenderPlan& plan,
                                                  ProgressSink& progress,
                                                  LogSink& logs) {
    const auto* prepared = dynamic_cast<const VbapPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error, "saf-vbap: incompatible prepared state");
    }
    return render_common::render_speaker_pcm(plan, prepared->mix, prepared->outputs, "saf-vbap", progress, logs);
}

} // namespace

bool register_vbap_layout(std::string id, std::string display_name, std::vector<VbapSpeakerSpec> speakers) {
    if (id.empty() || speakers.empty() || speakers.size() == 2U) {
        return false;
    }
    if (std::ranges::all_of(speakers, [](const auto& s) { return s.is_lfe; })) {
        return false; // nothing to pan
    }
    if (std::ranges::any_of(speakers,
                            [](const auto& s) { return !std::isfinite(s.azimuth) || !std::isfinite(s.elevation); })) {
        return false;
    }
    if (render_layouts::find_speaker_layout(id) != nullptr) {
        return false;
    }
    auto& reg = custom_layout_registry();
    if (std::ranges::any_of(reg, [&id](const RegistryEntry& e) { return e.id == id; })) {
        return false;
    }
    reg.push_back({std::move(id), std::move(display_name), std::move(speakers)});
    return true;
}

CapabilityReport vbap_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-vbap";
    r.backend_version = "1.3.4";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_screen_ref = false;
    r.supports_diffuse = false;
    r.supports_render_window = true; // direct seek; no DSP state, no pre-roll needed

    auto append_layout = [&](std::string_view id, std::string_view display_name, const auto& speakers) {
        CapabilityReport::Layout layout;
        layout.id = std::string{id};
        layout.display_name = std::string{display_name};
        layout.channel_count = static_cast<uint16_t>(speakers.size());
        layout.lfe_count =
            static_cast<uint16_t>(std::ranges::count_if(speakers, [](const auto& s) { return s.is_lfe; }));
        layout.is_3d =
            std::ranges::any_of(speakers, [](const auto& s) { return !s.is_lfe && std::fabs(s.elevation) > 1.0e-6F; });
        layout.supports_spread = layout.is_3d; // 2D VBAP passes spread=0; MDAP/SAF only helps for 3D
        r.supported_layouts.push_back(std::move(layout));
    };

    for (const auto& entry : render_layouts::speaker_layouts()) {
        append_layout(entry.id, entry.display_name, entry.speakers);
    }
    for (const auto& entry : custom_layout_registry()) {
        append_layout(entry.id, entry.display_name, entry.speakers);
    }
    return r;
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
