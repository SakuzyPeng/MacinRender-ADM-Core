#include "live_rust_renderer.h"

#include <algorithm>
#include <span>
#include <utility>

#include "live_scene_trace.h"
#include "speaker_pcm.h"

namespace mradm::live_scene {

namespace {

using render_common::text;

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

// Generations, frame staging, panning, warnings and the `.30-*` checkpoints are Rust
// mradm-render `live_vbap` / `live_triple_balance` (PCM consistency 4.5, 4.6); this shell converts
// the frame records.
class LiveRustRenderer final : public ILiveSceneRenderer {
  public:
    LiveRustRenderer(MradmRenderLive* handle, uint32_t sample_rate, DiagnosticSink diagnostics, std::string suffix)
        : handle_(handle, &mradm_render_live_destroy), sample_rate_(sample_rate), diagnostics_(std::move(diagnostics)),
          suffix_(std::move(suffix)) {}

    [[nodiscard]] Result<void> configure_generation(std::uint64_t generation_id,
                                                    std::span<const ElementDescriptor> elements) override {
        std::vector<MradmRenderLiveElement> records;
        records.reserve(elements.size());
        std::size_t labels = 0;
        for (const auto& e : elements) {
            const std::string_view object_id =
                e.semantic_identity ? e.semantic_identity->object_id : std::string_view{};
            records.push_back({e.element_id,
                               static_cast<uint32_t>(e.role),
                               text(e.speaker_label),
                               text(object_id),
                               e.has_position ? 1U : 0U,
                               {e.x, e.y, e.z}});
            labels += e.speaker_label.size();
        }
        RustError error(labels);
        const auto code = mradm_render_live_configure(handle_.get(),
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

    void reset() override { mradm_render_live_reset(handle_.get()); }

    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        planes_.resize(frame.pcm.size());
        initial_.resize(frame.initial_states.size());
        updates_.resize(frame.updates.size());
        std::ranges::transform(frame.pcm, planes_.begin(), [](const auto& plane) -> MradmRenderLivePlane {
            return {plane.element_id, plane.has_signal ? 1U : 0U, plane.samples.data(), plane.samples.size()};
        });
        std::ranges::transform(frame.initial_states, initial_.begin(), [](const auto& entry) -> MradmRenderLiveEntry {
            return {entry.element_id, state_record(entry.state)};
        });
        std::ranges::transform(frame.updates, updates_.begin(), [](const auto& update) -> MradmRenderLiveUpdate {
            return {update.element_id,
                    update.offset_samples,
                    update.ramp_duration_samples,
                    update.jump_position ? 1U : 0U,
                    update.changed_fields,
                    update.cleared_fields,
                    state_record(update.state)};
        });
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        trace_ = consistency::scene_renderer_key(frame) + suffix_;
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
        const auto code = mradm_render_live_render(handle_.get(),
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
        return mradm_render_live_channels(handle_.get());
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return sample_rate_; }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override { return 0U; }

  private:
    std::unique_ptr<MradmRenderLive, decltype(&mradm_render_live_destroy)> handle_;
    uint32_t sample_rate_;
    DiagnosticSink diagnostics_;
    std::string suffix_;
    std::vector<MradmRenderLivePlane> planes_;
    std::vector<MradmRenderLiveEntry> initial_;
    std::vector<MradmRenderLiveUpdate> updates_;
    std::string trace_;
    RustError error_;
};

} // namespace

std::unique_ptr<ILiveSceneRenderer> adopt_live_renderer(MradmRenderLive* handle,
                                                        uint32_t sample_rate,
                                                        DiagnosticSink diagnostics,
                                                        std::string trace_suffix) {
    return std::make_unique<LiveRustRenderer>(handle, sample_rate, std::move(diagnostics), std::move(trace_suffix));
}

} // namespace mradm::live_scene
