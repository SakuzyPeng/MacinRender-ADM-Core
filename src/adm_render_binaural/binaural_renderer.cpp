#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "adm/render.h"
#include "adm/render_binaural.h"

#include "../adm_audio/wav_backend.h"
#include "capability_records.h"
#include "render_common.h"
#include "speaker_pcm.h"

namespace mradm {

// The HRTF state (4.7 A), the source plan, OLA convolution and the offline loop (4.7 B) and the
// file stream (4.7 C) are Rust mradm-render `binaural_hrtf`, `binaural`, `binaural_render` and
// `binaural_stream`; the preparation and offline windows run in mradm-engine (5.2 C1).
namespace binaural_internal {
namespace {

[[nodiscard]] uint32_t numerical_spread(BinauralSpreadMode mode) noexcept {
    switch (mode) {
    case BinauralSpreadMode::none:
        return 1U;
    case BinauralSpreadMode::saf_spreader:
        return 2U;
    default:
        return 0U;
    }
}

using render_common::EnginePrepared;

// File streaming over the same prepared HRTF state and source list as render_window. The FIFO,
// seek pre-roll, live gain ramps, coalesced topology crossfades and head tracking are Rust
// mradm-render `binaural_stream` (PCM consistency 4.7 C); this side owns cancellation. The owning
// factory keeps the prepared state alive for the stream's lifetime.
class BinauralStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<IRenderStream>> create(const EnginePrepared& prepared,
                                                                       const RenderPlan& plan) {
        const auto& info = plan.scene.info;
        const MradmRenderBinauralStreamSettings settings{info.num_channels,
                                                         info.num_frames,
                                                         info.sample_rate,
                                                         plan.object_smoothing_frames,
                                                         numerical_spread(plan.binaural_spread_mode)};
        const auto path = audio::wav_path_utf8(plan.input_path);
        MradmRenderBinauralStream* raw = nullptr;
        render_common::DriverError error;
        if (const auto code = mradm_render_binaural_stream_open(prepared.handle.get(),
                                                                reinterpret_cast<const uint8_t*>(path.data()),
                                                                path.size(),
                                                                &settings,
                                                                &raw,
                                                                error.location(),
                                                                error.message(),
                                                                error.message_len());
            code != 0) {
            return error.error(code, plan.input_path);
        }
        return std::unique_ptr<IRenderStream>{new BinauralStream(raw, plan.input_path)};
    }

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        render_common::DriverError error;
        std::size_t produced = 0;
        if (const auto code = mradm_render_binaural_stream_process(handle_.get(),
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

    [[nodiscard]] Result<void> seek(uint64_t frame) override { return seek_with_cancel(frame, {}); }

    [[nodiscard]] bool seek_requires_preroll() const override { return true; }

    [[nodiscard]] Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) override {
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "双耳文件流式跳转已取消");
        }
        render_common::DriverError error;
        uint32_t pending = 0;
        if (const auto code = mradm_render_binaural_stream_seek_begin(
                handle_.get(), frame, &pending, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        while (pending != 0U) {
            if (cancel.stop_requested()) {
                return make_error(ErrorCode::cancelled, "双耳文件流式跳转已取消");
            }
            if (const auto code = mradm_render_binaural_stream_seek_step(
                    handle_.get(), &pending, error.location(), error.message(), error.message_len());
                code != 0) {
                return error.error(code, input_path_);
            }
        }
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "双耳文件流式跳转已取消");
        }
        return {};
    }

    // Gain targets per semantic source key; topology edits rebuild the source graph at most every
    // 100 ms with a crossfade.
    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = render_common::live_override_records(overrides);
        if (mradm_render_binaural_stream_overrides(handle_.get(), records.data(), records.size()) != 0) {
            std::terminate();
        }
    }

    // Live listener head orientation: world-locked sources rotate into the head frame before the
    // HRTF lookup; head-locked sources ignore it.
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        mradm_render_binaural_stream_pose(
            handle_.get(), orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg);
    }

    [[nodiscard]] uint32_t out_channels() const override { return 2U; }
    [[nodiscard]] uint32_t sample_rate() const override { return 48000U; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }

  private:
    BinauralStream(MradmRenderBinauralStream* raw, std::string input_path)
        : handle_(raw, &mradm_render_binaural_stream_destroy), input_path_(std::move(input_path)) {}

    std::unique_ptr<MradmRenderBinauralStream, decltype(&mradm_render_binaural_stream_destroy)> handle_;
    std::string input_path_;
};

class BinauralRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return binaural_capabilities(); }

    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override {
        auto prepared = render_common::prepare_backend(render_common::EngineBackend::binaural, plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return std::static_pointer_cast<IPreparedRender>(*prepared);
    }

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const EnginePrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "binaural: open_stream received an incompatible prepared state", {});
        }
        (void) logs;
        return BinauralStream::create(*prepared, plan);
    }
};

} // namespace
} // namespace binaural_internal

CapabilityReport binaural_capabilities() {
    return render_common::engine_capabilities(render_common::EngineBackend::binaural);
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<binaural_internal::BinauralRenderer>();
}

bool binaural_sofa_supported() {
#if MR_ADM_ENABLE_SOFA
    return true;
#else
    return false;
#endif
}

} // namespace mradm
