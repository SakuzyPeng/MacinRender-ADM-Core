#include "live_vbap_renderer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "live_scene_trace.h"
#include "routing_ffi.h"
#include "speaker_pcm.h"

namespace mradm::live_scene {

namespace {

using render_common::text;

// Rust error text: fixed wording plus the input bytes, so `extra` (their size) always fits.
class RustError {
  public:
    explicit RustError(std::size_t extra = 0) : message_(extra + 512), context_(512) {}
    uint8_t* message() noexcept { return message_.data(); }
    uint8_t* context() noexcept { return context_.data(); }
    std::size_t* message_len() noexcept { return &(message_len_ = message_.size()); }
    std::size_t* context_len() noexcept { return &(context_len_ = context_.size()); }
    [[nodiscard]] tl::unexpected<Error> error(int32_t code) const {
        return make_error(static_cast<ErrorCode>(code),
                          std::string{reinterpret_cast<const char*>(message_.data()), message_len_},
                          std::string{reinterpret_cast<const char*>(context_.data()), context_len_});
    }

  private:
    std::vector<uint8_t> message_;
    std::vector<uint8_t> context_;
    std::size_t message_len_{0};
    std::size_t context_len_{0};
};

[[nodiscard]] MradmRenderLiveState state_record(const ObjectState& s) noexcept {
    return {s.valid_fields,
            s.active ? 1U : 0U,
            s.linear_gain,
            {s.x, s.y, s.z},
            s.width,
            s.height,
            s.depth,
            s.diffuse,
            s.divergence,
            s.channel_lock ? 1U : 0U,
            s.screen_reference ? 1U : 0U,
            s.head_locked ? 1U : 0U,
            s.divergence_azimuth_range,
            s.divergence_position_range,
            s.channel_lock_max_distance ? 1U : 0U,
            s.channel_lock_max_distance.value_or(0.0F)};
}

// Generations, frame staging, panning, warnings and the `.30-vbap` checkpoints are Rust
// mradm-render `live_vbap` (PCM consistency 4.5); this shell converts the frame records.
class LiveVbapRenderer final : public ILiveSceneRenderer {
  public:
    LiveVbapRenderer(MradmRenderLiveVbap* handle, RendererConfig config, DiagnosticSink diagnostics)
        : handle_(handle, &mradm_render_live_vbap_destroy), config_(std::move(config)),
          diagnostics_(std::move(diagnostics)) {}

    [[nodiscard]] Result<void> configure_generation(std::uint64_t generation_id,
                                                    std::span<const ElementDescriptor> elements) override {
        std::vector<MradmRenderLiveElement> records;
        records.reserve(elements.size());
        std::size_t labels = 0;
        for (const auto& e : elements) {
            records.push_back({e.element_id,
                               static_cast<uint32_t>(e.role),
                               text(e.speaker_label),
                               e.has_position ? 1U : 0U,
                               {e.x, e.y, e.z}});
            labels += e.speaker_label.size();
        }
        RustError error(labels);
        const auto code = mradm_render_live_vbap_configure(handle_.get(),
                                                           generation_id,
                                                           records.data(),
                                                           records.size(),
                                                           error.message(),
                                                           error.message_len(),
                                                           error.context(),
                                                           error.context_len());
        if (code != 0) {
            return error.error(code);
        }
        return {};
    }

    void reset() override { mradm_render_live_vbap_reset(handle_.get()); }

    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        planes_.clear();
        initial_.clear();
        updates_.clear();
        for (const auto& plane : frame.pcm) {
            planes_.push_back(
                {plane.element_id, plane.has_signal ? 1U : 0U, plane.samples.data(), plane.samples.size()});
        }
        for (const auto& entry : frame.initial_states) {
            initial_.push_back({entry.element_id, state_record(entry.state)});
        }
        for (const auto& update : frame.updates) {
            updates_.push_back({update.element_id,
                                update.offset_samples,
                                update.ramp_duration_samples,
                                update.jump_position ? 1U : 0U,
                                update.changed_fields,
                                update.cleared_fields,
                                state_record(update.state)});
        }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        trace_ = consistency::scene_renderer_key(frame) + ".30-vbap" + std::to_string(output_channels());
#endif
        const MradmRenderLiveFrame record{frame.generation_id,
                                          frame.duration_samples,
                                          planes_.data(),
                                          planes_.size(),
                                          initial_.data(),
                                          initial_.size(),
                                          updates_.data(),
                                          updates_.size(),
                                          text(trace_)};
        const MradmRenderLiveDiagnostic* list = nullptr;
        std::size_t count = 0;
        const auto code = mradm_render_live_vbap_render(handle_.get(),
                                                        &record,
                                                        output.data(),
                                                        output.size(),
                                                        &list,
                                                        &count,
                                                        error_.message(),
                                                        error_.message_len(),
                                                        error_.context(),
                                                        error_.context_len());
        if (code != 0) {
            return error_.error(code);
        }
        if (diagnostics_) {
            for (const auto& d : std::span{list, count}) {
                diagnostics_({LogLevel::warning,
                              static_cast<DiagnosticCode>(d.code),
                              frame.epoch_id,
                              frame.generation_id,
                              d.element_id,
                              d.field,
                              std::string{reinterpret_cast<const char*>(d.message.data), d.message.len}});
            }
        }
        return {};
    }

    [[nodiscard]] std::uint32_t output_channels() const noexcept override {
        return mradm_render_live_vbap_channels(handle_.get());
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override { return 0U; }

  private:
    std::unique_ptr<MradmRenderLiveVbap, decltype(&mradm_render_live_vbap_destroy)> handle_;
    RendererConfig config_;
    DiagnosticSink diagnostics_;
    std::vector<MradmRenderLivePlane> planes_;
    std::vector<MradmRenderLiveEntry> initial_;
    std::vector<MradmRenderLiveUpdate> updates_;
    std::string trace_;
    RustError error_;
};

} // namespace

Result<std::unique_ptr<ILiveSceneRenderer>> create_live_vbap_renderer(const RendererConfig& config,
                                                                      DiagnosticSink diagnostics) {
    const MradmRenderLiveVbapRequest request{text(config.output_layout),
                                             config.speaker_geometry == SpeakerGeometry::apple ? 1U : 0U,
                                             config.speaker_spread_mode != SpeakerSpreadMode::none ? 1U : 0U,
                                             config.lfe_routing_mode == LfeRoutingMode::split_power ? 1U : 0U,
                                             config.object_smoothing_frames};
    MradmRenderLiveVbap* raw = nullptr;
    RustError error(config.output_layout.size());
    const auto code = mradm_render_live_vbap_create(&request, &raw, error.message(), error.message_len());
    if (code != 0) {
        return error.error(code);
    }
    return std::unique_ptr<ILiveSceneRenderer>{std::make_unique<LiveVbapRenderer>(raw, config, std::move(diagnostics))};
}

} // namespace mradm::live_scene
