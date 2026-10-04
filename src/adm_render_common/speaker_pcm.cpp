#include "speaker_pcm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ebur128.h>
#include <future>
#include <memory>

#include <fmt/format.h>

#include "adm/audio_io.h"

#include "render_common.h"

namespace mradm::render_common {
namespace {
void accumulate_channel_block(const ChannelGainInfo& channel,
                              std::size_t& block_index,
                              const AccumulateContext& ctx,
                              std::size_t frame) {
    const uint64_t abs_frame = ctx.frames_done + frame;
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return;
    }

    const float in_sample = ctx.input[(frame * ctx.num_in_ch) + channel.input_channel];
    const uint64_t interp_len = render_common::interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;

    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        float gain = block.gains[out_ch];
        if (ramping) {
            gain = render_common::interpolated_scalar(
                channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len);
        }
        float contribution = (in_sample * gain) * channel.output_gain;
        if (!ctx.live_gains.empty()) {
            contribution *= ctx.live_gains[(frame * ctx.num_in_ch) + channel.input_channel];
        }
        (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += contribution;
    }
}

[[nodiscard]] bool gains_at_frame(const ChannelGainInfo& channel,
                                  std::size_t& block_index,
                                  const AccumulateContext& ctx,
                                  uint64_t abs_frame,
                                  std::vector<float>& gains) {
    std::ranges::fill(gains, 0.0F);
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return false;
    }

    const uint64_t interp_len = render_common::interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;
    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        gains[out_ch] = block.gains[out_ch];
        if (ramping) {
            gains[out_ch] = render_common::interpolated_scalar(
                channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len);
        }
    }
    return block.smoothable_object;
}

} // namespace

void accumulate_speaker_channel(const ChannelGainInfo& channel,
                                std::size_t& block_index,
                                const AccumulateContext& ctx,
                                uint64_t frames_now) {
    if (channel.blocks.empty()) {
        return;
    }
    for (std::size_t frame = 0; frame < frames_now; ++frame) {
        accumulate_channel_block(channel, block_index, ctx, frame);
    }
}

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now) {
    std::vector<float> start_gains(ctx.num_out_ch);
    std::vector<float> end_gains(ctx.num_out_ch);
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& channel = gain_matrix[ci];
        if (channel.blocks.empty()) {
            continue;
        }
        auto start_index = block_indices[ci];
        auto end_index = start_index;
        const bool smooth_start =
            ctx.object_smoothing_frames > 0 && gains_at_frame(channel, start_index, ctx, ctx.frames_done, start_gains);
        const bool smooth_end = ctx.object_smoothing_frames > 0 &&
                                gains_at_frame(channel, end_index, ctx, ctx.frames_done + frames_now - 1, end_gains);
        if (!smooth_start || !smooth_end) {
            for (std::size_t frame = 0; frame < frames_now; ++frame) {
                accumulate_channel_block(channel, block_indices[ci], ctx, frame);
            }
            continue;
        }
        block_indices[ci] = start_index;

        for (std::size_t frame = 0; frame < frames_now; ++frame) {
            const float alpha = frames_now > 1 ? static_cast<float>(frame) / static_cast<float>(frames_now - 1) : 0.0F;
            const float in_sample = ctx.input[(frame * ctx.num_in_ch) + channel.input_channel];
            for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
                const float gain = (start_gains[out_ch] * (1.0F - alpha)) + (end_gains[out_ch] * alpha);
                float contribution = (in_sample * gain) * channel.output_gain;
                if (!ctx.live_gains.empty()) {
                    contribution *= ctx.live_gains[(frame * ctx.num_in_ch) + channel.input_channel];
                }
                (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += contribution;
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-size): keep writer and asynchronous meter buffer lifetimes in one scope.
Result<RenderMetrics> render_speaker_pcm(const RenderPlan& plan,
                                         const std::vector<ChannelGainInfo>& gain_matrix,
                                         uint16_t num_out_ch,
                                         std::string_view backend,
                                         ProgressSink& progress,
                                         LogSink& logs,
                                         const SpeakerBlockProcessor& process) {
    const auto& info = plan.scene.info;
    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;
    try {
        logs.log(LogLevel::info,
                 backend,
                 fmt::format("rendering {} tracks (Objects + DirectSpeakers) → {} channels, {} frames",
                             gain_matrix.size(),
                             num_out_ch,
                             num_frames));
        progress.on_progress(
            {RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, "rendering audio"});

        auto reader_res = audio::RenderInputReader::open(plan.input_path,
                                                         plan.scene.info.source_kind == SceneSourceKind::channel_bed);
        if (!reader_res) {
            return tl::unexpected{reader_res.error()};
        }
        auto reader = std::move(*reader_res);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;

        // Current block index per channel — advanced monotonically as frames_done increases.
        std::vector<std::size_t> blk_idx(gain_matrix.size(), 0);

        struct EburFree {
            void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
        };
        using EburPtr = std::unique_ptr<ebur128_state, EburFree>;
        EburPtr lufs_st{
            ebur128_init(num_out_ch, static_cast<unsigned long>(sample_rate), EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);

        // Loudness / true-peak measurement dominates this renderer (≈70% at 22.2). Run it on a
        // background thread so it overlaps with the next block's read + mix. Double-buffer the output
        // so the next block can be mixed while the meter still reads the previous one; reuse of a
        // buffer waits on its outstanding measurement future. Block order is preserved on the worker,
        // so the measured loudness / true peak is identical to the inline version.
        constexpr std::size_t k_num_buffers = 2;
        std::array<std::vector<float>, k_num_buffers> out_buffers;
        for (auto& buffer : out_buffers) {
            buffer.assign(static_cast<std::size_t>(num_out_ch) * k_block_size, 0.0F);
        }
        std::array<std::future<void>, k_num_buffers> meter_pending;
        render_common::SerialWorker meter;
        std::size_t buf_idx = 0;

        // Stateful backend DSP warms from frame zero; stateless gain mixing can seek.
        // Only frames inside the requested window enter the writer and meter.
        const bool windowed = plan.render_window.has_value();
        const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
        const uint64_t win_end =
            windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
        const uint64_t start_pos = windowed && !process ? (win_start / k_block_size) * k_block_size : 0;
        if (start_pos > 0) {
            render_common::seek_reader_abs(*reader, start_pos);
        }
        const uint64_t progress_total = std::max<uint64_t>(1, win_end - start_pos);
        const auto progress_span = static_cast<double>(progress_total);
        uint64_t frames_done = start_pos;

        while (frames_done < win_end) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            // Reclaim this buffer once the meter has finished its previous use of it.
            if (meter_pending.at(buf_idx).valid()) {
                meter_pending.at(buf_idx).get();
            }
            std::vector<float>& out_block = out_buffers.at(buf_idx);

            if (reader->read(in_block.data(), frames_now) != frames_now) {
                return make_error(ErrorCode::io_error, "short input read while rendering speaker PCM");
            }
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            const AccumulateContext ctx{in_block.data(),
                                        &out_block,
                                        frames_done,
                                        num_in_ch,
                                        num_out_ch,
                                        k_default_interp,
                                        plan.object_smoothing_frames};
            accumulate_gain_matrix(gain_matrix, blk_idx, ctx, frames_now);
            if (process) {
                auto status =
                    process(std::span<const float>(in_block.data(), static_cast<std::size_t>(num_in_ch) * frames_now),
                            std::span<float>(out_block.data(), out_samples),
                            frames_done + frames_now == num_frames);
                if (!status) {
                    return tl::unexpected{status.error()};
                }
            }

            // Sub-range of this block inside the output window [win_start, win_end).
            const uint64_t w_lo = std::max(frames_done, win_start);
            const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
            const bool emit = w_hi > w_lo;
            const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
            const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;

            if (emit && writer.write(out_block.data() + (emit_off * num_out_ch), emit_count) != emit_count) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
            }

            // Meter the written frames. Windowed → exactly emit_count; otherwise honor meter_window.
            if (lufs_st) {
                std::size_t meter_off = 0;
                std::size_t meter_count = 0;
                if (windowed) {
                    meter_off = emit_off;
                    meter_count = emit_count;
                } else {
                    const auto chunk = render_common::meter_window_chunk(plan.meter_window, frames_done, frames_now);
                    meter_off = chunk.offset_frames;
                    meter_count = static_cast<std::size_t>(chunk.frame_count);
                }
                if (meter_count > 0) {
                    ebur128_state* state = lufs_st.get();
                    const float* data = out_block.data() + (meter_off * num_out_ch);
                    const auto frame_count = meter_count;
                    meter_pending.at(buf_idx) =
                        meter.post([state, data, frame_count] { ebur128_add_frames_float(state, data, frame_count); });
                }
            }

            frames_done += frames_now;
            buf_idx = (buf_idx + 1) % k_num_buffers;

            const uint64_t progress_done = std::min(frames_done, win_end) - start_pos;
            const double stage_fraction = static_cast<double>(progress_done) / progress_span;
            const double frac = 0.3 + (0.6 * stage_fraction);
            progress.on_progress({RenderStage::rendering,
                                  RenderOperation::render_audio,
                                  frac,
                                  stage_fraction,
                                  progress_done,
                                  progress_total,
                                  "rendering"});
        }

        // All audio is written; wait for outstanding measurements before querying global metrics.
        for (auto& pending : meter_pending) {
            if (pending.valid()) {
                pending.get();
            }
        }

        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        logs.log(LogLevel::info,
                 backend,
                 fmt::format("wrote {} frames to {}{}",
                             win_end - win_start,
                             plan.output_path,
                             windowed ? fmt::format(" (window [{}, {}) of {} frames)", win_start, win_end, num_frames)
                                      : std::string{}));

        RenderMetrics metrics;
        if (lufs_st) {
            double loudness = 0.0;
            if (ebur128_loudness_global(lufs_st.get(), &loudness) == EBUR128_SUCCESS && std::isfinite(loudness)) {
                metrics.measured_lufs = loudness;
            }
            double max_peak = 0.0;
            for (unsigned int ch = 0; ch < num_out_ch; ++ch) {
                double ch_peak = 0.0;
                if (ebur128_true_peak(lufs_st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, ch_peak);
                }
            }
            if (max_peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(max_peak);
            }
        }
        return metrics;
    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::io_error, fmt::format("{} render failed: {}", backend, e.what()), "input=" + plan.input_path);
    }
}

} // namespace mradm::render_common
