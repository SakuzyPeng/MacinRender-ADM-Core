#include "speaker_pcm.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <memory>

#include <fmt/format.h>

#include "adm/audio_io.h"

#include "../adm_audio/wav_backend.h"
#include "render_common.h"

namespace mradm::render_common {
tl::unexpected<Error> DriverError::error(int32_t code, const std::string& input_path) const {
    std::string context;
    if (location_ == 1U) {
        context = "path=" + input_path;
    } else if (location_ == 2U) {
        context = "input=" + input_path;
    } else if (location_ == 3U) {
        context = "meter";
    } else if (location_ == 4U) {
        context = "HOA DSP";
    } else if (location_ == 5U) {
        context = "Triple Balance DSP";
    } else if (location_ == 6U) {
        context = "room-binaural";
    }
    return make_error(static_cast<ErrorCode>(code),
                      std::string{reinterpret_cast<const char*>(message_.data()), message_len_},
                      std::move(context));
}

std::vector<MradmRenderOverride> override_records(const LiveOverrides& overrides) {
    std::vector<MradmRenderOverride> result;
    result.reserve(overrides.objects.size());
    std::ranges::transform(overrides.objects, std::back_inserter(result), [](const LiveObjectOverride& o) {
        return MradmRenderOverride{text(o.object_id),
                                   text(o.speaker_label),
                                   o.mute ? 1U : 0U,
                                   o.gain_db,
                                   o.head_locked.has_value() ? 1U : 0U,
                                   o.head_locked.value_or(false) ? 1U : 0U};
    });
    return result;
}

std::vector<MradmRenderLiveOverride> live_override_records(const LiveOverrides& overrides) {
    const auto base = override_records(overrides);
    std::vector<MradmRenderLiveOverride> result;
    result.reserve(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        const auto& o = overrides.objects[i];
        result.push_back({base[i],
                          o.extent_scale,
                          o.extent_width_scale,
                          o.extent_height_scale,
                          o.extent_depth_scale,
                          o.diffuse_scale,
                          o.divergence_scale});
    }
    return result;
}

MatrixRecords::MatrixRecords(const DirectSpeakersMatrix& matrix) {
    targets.reserve(matrix.routes.size());
    for (const auto& route : matrix.routes) {
        auto& records = targets.emplace_back();
        for (const auto& target : route.targets) {
            records.push_back({text(target.label), target.gain});
            label_bytes += target.label.size();
        }
        routes.push_back({text(route.source_label), route.mute ? 1U : 0U, records.data(), records.size()});
        label_bytes += route.source_label.size();
    }
}

Result<std::shared_ptr<RustPrepared>> adopt_prepared(int32_t code,
                                                     MradmRenderPrepared* raw,
                                                     const MradmRenderPreparedView& view,
                                                     DriverStyle style,
                                                     LogSink& logs,
                                                     std::string_view module) {
    auto prepared = std::make_shared<RustPrepared>();
    prepared->handle.reset(raw);
    if (raw == nullptr) {
        return make_error(static_cast<ErrorCode>(code), std::string{module} + " preparation failed");
    }
    const auto string_of = [](MradmAdmText t) { return std::string{reinterpret_cast<const char*>(t.data), t.len}; };
    for (const auto& line : std::span{view.logs, view.logs_len}) {
        logs.log(static_cast<LogLevel>(line.level), module, string_of(line.message));
    }
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), string_of(view.message), string_of(view.context));
    }
    prepared->mix = {view.plan, view.tracks, style};
    prepared->outputs = static_cast<uint16_t>(view.outputs);
    return prepared;
}

namespace {

class FileStream final : public IRenderStream {
  public:
    FileStream(MradmRenderStream* raw, const RenderPlan& plan, uint16_t outputs)
        : handle_(raw, &mradm_render_stream_destroy), input_path_(plan.input_path), outputs_(outputs),
          sample_rate_(plan.scene.info.sample_rate), output_layout_(plan.output_layout) {}

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        DriverError error;
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
        DriverError error;
        if (const auto code =
                mradm_render_stream_seek(handle_.get(), frame, error.location(), error.message(), error.message_len());
            code != 0) {
            return error.error(code, input_path_);
        }
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        const auto records = override_records(overrides);
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

} // namespace

Result<std::unique_ptr<IRenderStream>> open_file_stream(const RustPrepared& prepared, const RenderPlan& plan) {
    const auto path = audio::wav_path_utf8(plan.input_path);
    DriverError error;
    MradmRenderStream* raw = nullptr;
    if (const auto code = mradm_render_stream_open(prepared.handle.get(),
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
    return std::unique_ptr<IRenderStream>{std::make_unique<FileStream>(raw, plan, prepared.outputs)};
}

namespace {

struct DriverText {
    std::string_view begin;
    std::string_view step;
    std::string_view short_write;
};
constexpr DriverText k_speaker_text{"rendering audio", "rendering", "short write while rendering"};
constexpr DriverText k_hoa_text{"encoding HOA", "encoding", "short write while encoding HOA"};

[[nodiscard]] std::string begin_log(DriverStyle style, std::size_t tracks, uint16_t outputs, uint64_t frames) {
    switch (style) {
    case DriverStyle::hoa:
        return fmt::format(
            "encoding {} input channels (Objects + DirectSpeakers) → HOA3 ({} ch), {} frames", tracks, outputs, frames);
    case DriverStyle::ear:
        return fmt::format("rendering {} tracks → {} channels, {} frames", tracks, outputs, frames);
    case DriverStyle::speaker:
        break;
    }
    return fmt::format(
        "rendering {} tracks (Objects + DirectSpeakers) → {} channels, {} frames", tracks, outputs, frames);
}

[[nodiscard]] std::string failure(DriverStyle style, std::string_view backend, const char* what) {
    switch (style) {
    case DriverStyle::hoa:
        return fmt::format("HOA encode failed: {}", what);
    case DriverStyle::ear:
        return fmt::format("render failed: {}", what);
    case DriverStyle::speaker:
        break;
    }
    return fmt::format("{} render failed: {}", backend, what);
}

} // namespace

// The loop itself runs in Rust mradm-render `offline` (PCM consistency 4.5): input reading,
// mixing (with Triple Balance's size or D-mode processor, 4.6), checkpoints and the asynchronous
// meter. This side owns the writer, cancellation, progress and logs.
Result<RenderMetrics> render_speaker_pcm(const RenderPlan& plan,
                                         const SpeakerMix& gain_matrix,
                                         uint16_t num_out_ch,
                                         std::string_view backend,
                                         ProgressSink& progress,
                                         LogSink& logs) {
    const auto& info = plan.scene.info;
    const auto style = gain_matrix.style;
    const auto& wording = style == DriverStyle::hoa ? k_hoa_text : k_speaker_text;
    try {
        logs.log(LogLevel::info, backend, begin_log(style, gain_matrix.tracks, num_out_ch, info.num_frames));
        progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, wording.begin});

        const auto& window = plan.render_window;
        const auto& meter_window = plan.meter_window;
        const MradmRenderOfflineSettings settings{info.num_channels,
                                                  num_out_ch,
                                                  info.num_frames,
                                                  static_cast<uint32_t>(info.sample_rate),
                                                  plan.default_interp_ms,
                                                  plan.object_smoothing_frames,
                                                  window ? 1U : 0U,
                                                  window ? window->start_frame : 0,
                                                  window ? window->frame_count : 0,
                                                  meter_window ? 1U : 0U,
                                                  meter_window ? meter_window->start_frame : 0,
                                                  meter_window ? meter_window->frame_count : 0};
        const auto path = audio::wav_path_utf8(plan.input_path);
        DriverError error;
        MradmRenderOffline* raw = nullptr;
        const auto code = mradm_render_offline_open(reinterpret_cast<const uint8_t*>(path.data()),
                                                    path.size(),
                                                    gain_matrix.plan,
                                                    &settings,
                                                    {reinterpret_cast<const uint8_t*>(backend.data()), backend.size()},
                                                    &raw,
                                                    error.location(),
                                                    error.message(),
                                                    error.message_len());
        if (code != 0) {
            return error.error(code, plan.input_path);
        }
        const std::unique_ptr<MradmRenderOffline, decltype(&mradm_render_offline_destroy)> driver(
            raw, &mradm_render_offline_destroy);
        auto writer = audio::WriterHandle::open(
            plan.output_path, num_out_ch, static_cast<uint32_t>(info.sample_rate), plan.output_layout);
        if (!writer) {
            return tl::unexpected{writer.error()};
        }
        for (;;) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            MradmRenderOfflineBlock block{};
            if (const auto status = mradm_render_offline_next(
                    driver.get(), &block, error.location(), error.message(), error.message_len());
                status != 0) {
                return error.error(status, plan.input_path);
            }
            if (block.frames == 0) {
                break;
            }
            if (block.emit_frames > 0 && writer->write(block.output + (block.emit_offset * num_out_ch),
                                                       block.emit_frames) != block.emit_frames) {
                return make_error(ErrorCode::io_error, std::string{wording.short_write}, "output=" + plan.output_path);
            }
            MradmRenderOfflineProgress step{};
            if (const auto status = mradm_render_offline_commit(
                    driver.get(), &step, error.location(), error.message(), error.message_len());
                status != 0) {
                return error.error(status, plan.input_path);
            }
            progress.on_progress({RenderStage::rendering,
                                  RenderOperation::render_audio,
                                  step.fraction,
                                  step.stage_fraction,
                                  step.done,
                                  step.total,
                                  wording.step});
        }
        MradmRenderOfflineMetrics measured{};
        if (const auto status = mradm_render_offline_finish(
                driver.get(), &measured, error.location(), error.message(), error.message_len());
            status != 0) {
            return error.error(status, plan.input_path);
        }

        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        const uint64_t start = window ? std::min(window->start_frame, info.num_frames) : 0;
        const uint64_t end = window ? start + std::min(window->frame_count, info.num_frames - start) : info.num_frames;
        logs.log(LogLevel::info,
                 backend,
                 fmt::format("wrote {} frames to {}{}",
                             end - start,
                             plan.output_path,
                             window ? fmt::format(" (window [{}, {}) of {} frames)", start, end, info.num_frames)
                                    : std::string{}));

        RenderMetrics metrics;
        if (measured.has_lufs != 0U) {
            metrics.measured_lufs = measured.lufs;
        }
        if (measured.has_peak != 0U) {
            metrics.measured_peak_dbtp = measured.peak_dbtp;
        }
        return metrics;
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, failure(style, backend, e.what()), "input=" + plan.input_path);
    }
}

} // namespace mradm::render_common
