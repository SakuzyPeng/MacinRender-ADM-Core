#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

#include "../adm_audio/wav_backend.h"
#include "hrtf_state.h"
#include "render_common.h"
#include "scene_handle.h"
#include "speaker_pcm.h"

namespace mradm {

using namespace binaural_internal;

// OLA convolution and the offline loop (4.7 B), and the file stream (4.7 C) are Rust mradm-render
// OLA convolution and the offline loop (4.7 B) and the file stream (4.7 C) are Rust mradm-render
// `binaural_hrtf`, `binaural`, `binaural_render` and `binaural_stream`.
namespace binaural_internal {
namespace {

constexpr uint64_t k_min_block_size = 1024U;

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

// The HRTF state and the Rust plan, reused across render_window() calls (PreviewSession scrubbing);
// the per-output convolution and spreader state is created per render.
struct BinauralPrepared final : IPreparedRender {
    std::shared_ptr<const HrtfState> bs;
    std::shared_ptr<render_common::RustPrepared> rust;
};

// File streaming over the same prepared HRTF state and source list as render_window. The FIFO,
// seek pre-roll, live gain ramps, coalesced topology crossfades and head tracking are Rust
// mradm-render `binaural_stream` (PCM consistency 4.7 C); this side owns cancellation. The owning
// factory keeps the prepared state alive for the stream's lifetime.
class BinauralStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<IRenderStream>> create(const BinauralPrepared& prepared,
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
        if (const auto code = mradm_render_binaural_stream_open(prepared.rust->handle.get(),
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
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const BinauralPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "binaural: open_stream received an incompatible prepared state", {});
        }
        (void) logs;
        return BinauralStream::create(*prepared, plan);
    }
};

CapabilityReport BinauralRenderer::capabilities() const {
    return binaural_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> BinauralRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic) {
        return make_error(ErrorCode::unsupported,
                          "SAF binaural renderer does not support explicit DirectSpeakers routing; use automatic",
                          {});
    }
    auto lfe_routing = render_common::resolve_lfe_routing(plan, logs, "binaural");
    if (!lfe_routing) {
        return tl::unexpected{lfe_routing.error()};
    }
    const auto& info = plan.scene.info;

    if (info.sample_rate != 48000U) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("binaural renderer requires 48000 Hz input, got {}", info.sample_rate),
                          "input=" + plan.input_path);
    }

    const uint64_t render_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);

    // One-time setup: load the HRIR dataset, then build the HRTF table and interpolation grid.
    auto state_result = HrtfState::prepare(plan.sofa_path, info.sample_rate, render_block_size);
    if (!state_result) {
        return tl::unexpected{state_result.error()};
    }
    auto bs = std::move(*state_result);
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("HRTF source: {} ({} dirs, {} taps @ {} Hz)",
                         bs->dataset_name,
                         bs->num_dirs,
                         bs->hrir_len,
                         info.sample_rate));
    if (plan.scene_handle == nullptr) {
        return make_error(ErrorCode::internal_error, "binaural requires the Rust render scene");
    }

    const MradmRenderBinauralRequest request{numerical_spread(plan.binaural_spread_mode), plan.object_smoothing_frames};
    MradmRenderPrepared* raw = nullptr;
    MradmRenderPreparedView view{};
    const auto code = mradm_render_binaural_prepare(plan.scene_handle->raw, bs->handle.get(), &request, &raw, &view);
    auto rust = render_common::adopt_prepared(code, raw, view, render_common::DriverStyle::speaker, logs, "binaural");
    if (!rust) {
        return tl::unexpected{rust.error()};
    }
    auto prepared = std::make_shared<BinauralPrepared>();
    prepared->bs = std::move(bs);
    prepared->rust = std::move(*rust);
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

Result<RenderMetrics> BinauralRenderer::render_window(const IPreparedRender& prep,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) {
    const auto* prepared = dynamic_cast<const BinauralPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(
            ErrorCode::internal_error, "binaural: render_window received an incompatible prepared state", {});
    }
    const auto& info = plan.scene.info;
    const uint64_t num_frames = info.num_frames;
    const bool windowed = plan.render_window.has_value();
    const auto& pose = plan.listener_orientation;
    const MradmRenderBinauralOfflineSettings settings{info.num_channels,
                                                      num_frames,
                                                      info.sample_rate,
                                                      windowed ? 1U : 0U,
                                                      windowed ? plan.render_window->start_frame : 0U,
                                                      windowed ? plan.render_window->frame_count : 0U,
                                                      plan.object_smoothing_frames,
                                                      pose.is_identity() ? 0U : 1U,
                                                      {pose.yaw_deg, pose.pitch_deg, pose.roll_deg}};

    // The input, the per-output (primed) spreader adapters, the OLA state and the meter.
    const auto path = audio::wav_path_utf8(plan.input_path);
    MradmRenderBinauralOffline* raw = nullptr;
    MradmAdmText pools{};
    render_common::DriverError error;
    if (const auto code = mradm_render_binaural_offline_open(prepared->rust->handle.get(),
                                                             reinterpret_cast<const uint8_t*>(path.data()),
                                                             path.size(),
                                                             &settings,
                                                             &raw,
                                                             &pools,
                                                             error.location(),
                                                             error.message(),
                                                             error.message_len());
        code != 0) {
        return error.error(code, plan.input_path);
    }
    const std::unique_ptr<MradmRenderBinauralOffline, decltype(&mradm_render_binaural_offline_destroy)> driver(
        raw, &mradm_render_binaural_offline_destroy);
    auto writer_res = audio::WriterHandle::open(plan.output_path, 2U, info.sample_rate, "binaural");
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;
    if (pools.len != 0U) {
        logs.log(LogLevel::info, "binaural", std::string{reinterpret_cast<const char*>(pools.data), pools.len});
    }

    progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.2, 0.0, 0, 0, "rendering"});
    while (const auto pending = mradm_render_binaural_offline_pending(driver.get())) {
        if (pending == 1U && plan.cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
        }
        MradmRenderBinauralStep step{};
        if (const auto code = mradm_render_binaural_offline_next(
                driver.get(), &step, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, plan.input_path);
        }
        if (step.emit != 0U && writer.write(step.output, step.emit) != step.emit) {
            return make_error(ErrorCode::io_error,
                              step.pending == 2U ? "short write during binaural spreader tail"
                                                 : "short write during binaural render",
                              "output=" + plan.output_path);
        }
        if (step.has_progress != 0U) {
            progress.on_progress({RenderStage::rendering,
                                  RenderOperation::render_audio,
                                  step.progress.fraction,
                                  step.progress.stage_fraction,
                                  step.progress.done,
                                  step.progress.total,
                                  "rendering"});
        }
    }
    uint64_t out_written = 0;
    MradmRenderOfflineMetrics measured{};
    (void) mradm_render_binaural_offline_finish(driver.get(), &out_written, &measured);
    if (auto finished = writer.finish(); !finished) {
        return tl::unexpected{finished.error()};
    }

    progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
    const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
    const uint64_t win_end = windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("wrote {} frames to {}{}",
                         out_written,
                         plan.output_path,
                         windowed ? fmt::format(" (window [{}, {}) of {} frames)", win_start, win_end, num_frames)
                                  : std::string{}));

    RenderMetrics metrics;
    if (measured.has_lufs != 0U) {
        metrics.measured_lufs = measured.lufs;
    }
    if (measured.has_peak != 0U) {
        // Rust keeps the binaural backend's established -200 dB silent floor.
        metrics.measured_peak_dbtp = measured.peak_dbtp;
    }
    return metrics;
}

} // namespace
} // namespace binaural_internal

CapabilityReport binaural_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-binaural-hrtf";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    r.hrtf_sources = {"built-in"};
#if MR_ADM_ENABLE_SOFA
    r.hrtf_sources.emplace_back("user-sofa");
#endif
    // Default (cloud/none) path windows via seek + aligned pre-roll; the experimental
    // saf_spreader path keeps full STFT warm-up and only trims output.
    r.supports_render_window = true;
    r.supported_layouts = {
        // clang-format off
        {"binaural", "SAF HRTF binaural (KEMAR or user SOFA HRIR)", 2, false, 0, true, true},
        // clang-format on
    };
    return r;
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<BinauralRenderer>();
}

bool binaural_sofa_supported() {
#if MR_ADM_ENABLE_SOFA
    return true;
#else
    return false;
#endif
}

} // namespace mradm
