#include "speaker_pcm.h"

#include <algorithm>
#include <iterator>
#include <memory>

#include <fmt/format.h>

#include "adm/audio_io.h"

#include "../adm_audio/wav_backend.h"
#include "render_common.h"

namespace mradm::render_common {
Result<PreparedPcmMix>
prepare_speaker_mix(std::vector<ChannelGainInfo> channels, std::size_t inputs, std::size_t outputs) {
    PreparedPcmMix result;
    std::vector<MradmDspMixRow> rows;
    std::vector<MradmDspMixBlock> blocks;
    std::vector<float> gains;
    for (auto& channel : channels) {
        result.channels.push_back(
            {channel.input_channel, std::move(channel.object_id), std::move(channel.speaker_label_key)});
        rows.push_back({channel.input_channel, blocks.size(), channel.blocks.size(), channel.output_gain});
        for (auto& block : channel.blocks) {
            if (block.gains.size() != outputs) {
                return make_error(ErrorCode::invalid_argument, "PCM gain width mismatch");
            }
            // Object-duration clipping can put the end before the start. Preserve an empty block
            // (and its gains) so later blocks retain the legacy interpolation predecessor.
            blocks.push_back({block.start_sample,
                              std::max(block.start_sample, block.end_sample),
                              block.interp_length_samples.value_or(0),
                              (block.jump_position ? 1U : 0U) | (block.smoothable_object ? 2U : 0U) |
                                  (block.interp_length_samples ? 4U : 0U)});
            gains.insert(gains.end(), block.gains.begin(), block.gains.end());
        }
        std::vector<BlockGains>{}.swap(channel.blocks);
    }
    auto prepared = dsp::PcmMixPlan::create(inputs, outputs, rows, blocks, gains);
    if (!prepared) {
        return tl::unexpected{prepared.error()};
    }
    result.plan = std::move(*prepared);
    return result;
}

tl::unexpected<Error> DriverError::error(int32_t code, const std::string& input_path) const {
    std::string context;
    if (location_ == 1U) {
        context = "path=" + input_path;
    } else if (location_ == 2U) {
        context = "input=" + input_path;
    } else if (location_ == 3U) {
        context = "meter";
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

// The loop itself runs in Rust mradm-render `offline` (PCM consistency 4.5): input reading,
// mixing, checkpoints and the asynchronous meter. This side owns the writer, cancellation,
// progress, logs and the optional stateful processor between two Rust steps.
Result<RenderMetrics> render_speaker_pcm(const RenderPlan& plan,
                                         SpeakerMix gain_matrix,
                                         uint16_t num_out_ch,
                                         std::string_view backend,
                                         ProgressSink& progress,
                                         LogSink& logs,
                                         const SpeakerBlockProcessor& process) {
    const auto& info = plan.scene.info;
    try {
        logs.log(LogLevel::info,
                 backend,
                 fmt::format("rendering {} tracks (Objects + DirectSpeakers) → {} channels, {} frames",
                             gain_matrix.tracks,
                             num_out_ch,
                             info.num_frames));
        progress.on_progress(
            {RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, "rendering audio"});

        const auto& window = plan.render_window;
        const auto& meter_window = plan.meter_window;
        const MradmRenderOfflineSettings settings{info.num_channels,
                                                  num_out_ch,
                                                  info.num_frames,
                                                  static_cast<uint32_t>(info.sample_rate),
                                                  plan.default_interp_ms,
                                                  plan.object_smoothing_frames,
                                                  window ? 1U : 0U,
                                                  process ? 1U : 0U,
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
            if (process) {
                auto status =
                    process({block.input, block.input_len}, {block.output, block.output_len}, block.last != 0);
                if (!status) {
                    return tl::unexpected{status.error()};
                }
            }
            if (block.emit_frames > 0 && writer->write(block.output + (block.emit_offset * num_out_ch),
                                                       block.emit_frames) != block.emit_frames) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
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
                                  "rendering"});
        }
        MradmRenderOfflineMetrics measured{};
        if (const auto status = mradm_render_offline_finish(
                driver.get(), &measured, error.location(), error.message(), error.message_len());
            status != 0) {
            return error.error(status, plan.input_path);
        }

        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        const uint64_t win_start = window ? std::min(window->start_frame, info.num_frames) : 0;
        const uint64_t win_end = window ? std::min(win_start + window->frame_count, info.num_frames) : info.num_frames;
        logs.log(LogLevel::info,
                 backend,
                 fmt::format("wrote {} frames to {}{}",
                             win_end - win_start,
                             plan.output_path,
                             window
                                 ? fmt::format(" (window [{}, {}) of {} frames)", win_start, win_end, info.num_frames)
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
        return make_error(
            ErrorCode::io_error, fmt::format("{} render failed: {}", backend, e.what()), "input=" + plan.input_path);
    }
}

} // namespace mradm::render_common
