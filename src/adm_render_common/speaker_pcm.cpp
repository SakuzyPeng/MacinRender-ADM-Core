#include "speaker_pcm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <memory>
#include <stdexcept>

#include <fmt/format.h>

#include "adm/audio_io.h"

#include "meter.h"
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

// NOLINTNEXTLINE(readability-function-size): keep writer and asynchronous meter buffer lifetimes in one scope.
Result<RenderMetrics> render_speaker_pcm(const RenderPlan& plan,
                                         const PreparedPcmMix& gain_matrix,
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


        auto lufs_st =
            dsp::Meter::create(num_out_ch, static_cast<uint32_t>(sample_rate), dsp::MeterMode::integrated_true_peak);
        if (!lufs_st) {
            return tl::unexpected{lufs_st.error()};
        }

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);
        dsp::PcmMixer mix(gain_matrix.plan,
                          static_cast<std::size_t>(k_block_size),
                          k_default_interp,
                          plan.object_smoothing_frames > 0);
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

            mix.speaker(std::span{in_block}.first(static_cast<std::size_t>(frames_now) * num_in_ch),
                        out_block,
                        {},
                        frames_done,
                        static_cast<std::size_t>(frames_now));
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
                    auto* state = &*lufs_st;
                    const float* data = out_block.data() + (meter_off * num_out_ch);
                    const auto frame_count = meter_count;
                    meter_pending.at(buf_idx) = meter.post([state, data, frame_count] {
                        if (const auto result = state->add_frames(data, frame_count); !result) {
                            throw std::runtime_error(result.error().message);
                        }
                    });
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
            if (const auto loudness = lufs_st->integrated(); loudness && std::isfinite(*loudness)) {
                metrics.measured_lufs = *loudness;
            }
            if (const auto peak = lufs_st->max_true_peak(); peak && *peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(*peak);
            }
        }
        return metrics;
    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::io_error, fmt::format("{} render failed: {}", backend, e.what()), "input=" + plan.input_path);
    }
}

} // namespace mradm::render_common
