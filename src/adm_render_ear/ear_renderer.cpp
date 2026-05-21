#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

#include <bw64/bw64.hpp>
#include <ear/ear.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_ear.h"

namespace mradm {

namespace {

struct BlockGains {
    std::vector<double> gains;
    std::vector<double> diffuse_gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<BlockGains> blocks; // sorted by start_sample
};

struct AccumulateContext {
    const float* input{nullptr};
    std::vector<float>* output{nullptr}; // direct output buffer (float)
    double* diffuse_in{nullptr};         // per-frame×ch diffuse input (double); may be nullptr
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
};

// FIR decorrelator state for the diffuse bus (BS.2127).
struct DecorrState {
    std::vector<std::vector<double>> filters;  // [num_out_ch][512]
    int comp_delay{0};                         // decorrelatorCompensationDelay() = 255
    std::vector<std::vector<double>> fir_hist; // [num_out_ch][511]  FIR overlap history
    std::vector<std::vector<float>> dir_delay; // [num_out_ch][comp_delay]  direct delay
};

[[nodiscard]] ear::ObjectsTypeMetadata object_metadata_from_block(const SceneObjectBlock& block,
                                                                  const SceneObject& obj) {
    ear::ObjectsTypeMetadata meta;

    const SceneBlockPosition pos =
        obj.position_offset ? apply_position_offset(block.position, *obj.position_offset) : block.position;

    if (pos.cartesian) {
        // BS.2076 §10.1: convert Cartesian (X right, Y front, Z up) to polar before
        // passing to libear — GainCalculatorObjects throws not_implemented("cartesian").
        const auto cx = static_cast<double>(pos.x);
        const auto cy = static_cast<double>(pos.y);
        const auto cz = static_cast<double>(pos.z);
        const double az = std::atan2(-cx, cy) * (180.0 / std::numbers::pi_v<double>);
        const double el = std::atan2(cz, std::sqrt((cx * cx) + (cy * cy))) * (180.0 / std::numbers::pi_v<double>);
        const double dist = std::sqrt((cx * cx) + (cy * cy) + (cz * cz));
        meta.position = ear::PolarPosition{az, el, dist};
        meta.cartesian = false;
    } else {
        meta.position = ear::PolarPosition{
            static_cast<double>(pos.azimuth),
            static_cast<double>(pos.elevation),
            static_cast<double>(pos.distance),
        };
        meta.cartesian = false;
    }
    meta.gain = static_cast<double>(block.gain) * static_cast<double>(obj.gain);
    meta.diffuse = static_cast<double>(block.diffuse);
    meta.width = static_cast<double>(block.width);
    meta.height = static_cast<double>(block.height);
    meta.depth = static_cast<double>(block.depth);
    return meta;
}

void warn_unsupported_object_fields(const SceneObjectBlock& block, LogSink& logs) {
    if (block.channel_lock) {
        logs.log(LogLevel::warning, "ear", "channelLock not supported by libear, degrading to unlocked");
    }
    if (block.divergence != 0.0F) {
        logs.log(LogLevel::warning,
                 "ear",
                 fmt::format("objectDivergence={:.3f} not supported by libear, degrading to 0", block.divergence));
    }
    if (block.screen_ref) {
        logs.log(LogLevel::warning, "ear", "screenRef not supported by libear, degrading to false");
    }
}

void append_object_blocks(const SceneTrackRef& track,
                          const SceneObject& obj,
                          ChannelGainInfo& cg,
                          ear::GainCalculatorObjects& objects_calc,
                          std::size_t num_out,
                          LogSink& logs) {
    for (const auto& block : track.blocks) {
        // P2 defensive layer: warn and degrade fields that cause libear
        // to throw not_implemented so the file doesn't fail to render.
        warn_unsupported_object_fields(block, logs);

        auto meta = object_metadata_from_block(block, obj);
        // meta.channelLock / objectDivergence / screenRef remain at their
        // default (unlocked / 0 / false) — do not set from block.

        BlockGains bg;
        bg.gains.resize(num_out, 0.0);
        bg.diffuse_gains.resize(num_out, 0.0);
        bg.start_sample = block.start_sample;
        bg.end_sample = std::min(block.end_sample, obj.end_sample);
        bg.jump_position = block.jump_position;
        bg.interp_length_samples = block.interp_length_samples;
        objects_calc.calculate(meta, bg.gains, bg.diffuse_gains);
        cg.blocks.push_back(std::move(bg));
    }
}

[[nodiscard]] ear::DirectSpeakersTypeMetadata direct_speakers_metadata_from_block(const SceneDirectSpeakersBlock& ds) {
    ear::DirectSpeakersTypeMetadata meta;
    meta.speakerLabels = ds.speaker_labels;
    // libear throws if audioPackFormatID is set without speaker labels (including
    // non-common-definition IDs). Only pass the ID when labels are also present so
    // that label-less custom DS blocks fall through to position-based routing.
    if (!ds.pack_format_id.empty() && !ds.speaker_labels.empty()) {
        meta.audioPackFormatID = ds.pack_format_id;
    }
    if (ds.has_position) {
        ear::PolarSpeakerPosition psp{
            static_cast<double>(ds.azimuth),
            static_cast<double>(ds.elevation),
            static_cast<double>(ds.distance),
        };
        if (ds.azimuth_min) {
            psp.azimuthMin = static_cast<double>(*ds.azimuth_min);
        }
        if (ds.azimuth_max) {
            psp.azimuthMax = static_cast<double>(*ds.azimuth_max);
        }
        if (ds.elevation_min) {
            psp.elevationMin = static_cast<double>(*ds.elevation_min);
        }
        if (ds.elevation_max) {
            psp.elevationMax = static_cast<double>(*ds.elevation_max);
        }
        if (ds.distance_min) {
            psp.distanceMin = static_cast<double>(*ds.distance_min);
        }
        if (ds.distance_max) {
            psp.distanceMax = static_cast<double>(*ds.distance_max);
        }
        meta.position = psp;
    }
    if (ds.low_pass_hz) {
        meta.channelFrequency.lowPass = static_cast<double>(*ds.low_pass_hz);
    }
    return meta;
}

void append_direct_speakers_blocks(const SceneTrackRef& track,
                                   const SceneObject& obj,
                                   ChannelGainInfo& cg,
                                   ear::GainCalculatorDirectSpeakers& direct_speakers_calc,
                                   std::size_t num_out) {
    for (const auto& ds : track.ds_blocks) {
        auto meta = direct_speakers_metadata_from_block(ds);
        BlockGains bg;
        bg.gains.resize(num_out, 0.0);
        bg.diffuse_gains.resize(num_out, 0.0); // DS has no diffuse bus
        bg.start_sample = ds.start_sample;
        bg.end_sample = std::min(ds.end_sample, obj.end_sample);
        bg.jump_position = true;
        direct_speakers_calc.calculate(meta, bg.gains);
        const auto ds_gain = static_cast<double>(ds.gain) * static_cast<double>(obj.gain);
        std::ranges::transform(bg.gains, bg.gains.begin(), [ds_gain](double g) { return g * ds_gain; });
        cg.blocks.push_back(std::move(bg));
    }
}

void append_hoa_blocks(const SceneHOATracks& pack,
                       std::map<uint16_t, ChannelGainInfo>& by_channel,
                       ear::GainCalculatorHOA& hoa_calc,
                       std::size_t num_out) {
    const std::size_t n_hoa = pack.channels.size();
    if (n_hoa == 0) {
        return;
    }

    ear::HOATypeMetadata meta;
    meta.normalization = pack.normalization;
    meta.nfcRefDist = pack.nfc_ref_dist;
    meta.screenRef = pack.screen_ref;
    meta.orders.resize(n_hoa);
    meta.degrees.resize(n_hoa);
    for (std::size_t i = 0; i < n_hoa; ++i) {
        meta.orders[i] = pack.channels[i].order;
        meta.degrees[i] = pack.channels[i].degree;
    }

    // decode_matrix[i][out_ch] = gain for HOA channel i → output channel out_ch.
    std::vector<std::vector<double>> decode_matrix(n_hoa, std::vector<double>(num_out, 0.0));
    hoa_calc.calculate(meta, decode_matrix);

    const auto obj_gain = static_cast<double>(pack.gain);

    for (std::size_t i = 0; i < n_hoa; ++i) {
        const auto& ch = pack.channels[i];
        if (!ch.channel_index.has_value()) {
            continue;
        }
        const uint16_t in_ch = *ch.channel_index;
        auto& cg = by_channel[in_ch];
        cg.input_channel = in_ch;

        // One BlockGains per AudioBlockFormatHoa block; the decode matrix row is
        // fixed (order/degree unchanged across blocks), only the gain scalar varies.
        for (const auto& hblk : ch.blocks) {
            const double ch_gain = static_cast<double>(hblk.gain) * obj_gain;
            BlockGains bg;
            bg.gains.resize(num_out, 0.0);
            bg.diffuse_gains.resize(num_out, 0.0);
            bg.start_sample = hblk.start_sample;
            bg.end_sample = hblk.end_sample;
            bg.jump_position = true; // decode matrix is static — no interpolation ramp
            for (std::size_t out_ch = 0; out_ch < num_out; ++out_ch) {
                bg.gains[out_ch] = decode_matrix[i][out_ch] * ch_gain;
            }
            cg.blocks.push_back(std::move(bg));
        }
    }
}

std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene, const ear::Layout& layout, LogSink& logs) {
    std::map<uint16_t, ChannelGainInfo> by_channel;
    ear::GainCalculatorObjects objects_calc{layout};
    ear::GainCalculatorDirectSpeakers direct_speakers_calc{layout};
    ear::GainCalculatorHOA hoa_calc{layout};
    const std::size_t num_out = layout.channels().size();

    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = *track.channel_index;
            auto& cg = by_channel[in_ch];
            cg.input_channel = in_ch;
            append_object_blocks(track, obj, cg, objects_calc, num_out, logs);
            append_direct_speakers_blocks(track, obj, cg, direct_speakers_calc, num_out);
        }
    }

    for (const auto& pack : scene.hoa_tracks) {
        if (!pack.mute) {
            append_hoa_blocks(pack, by_channel, hoa_calc, num_out);
        }
    }

    std::vector<ChannelGainInfo> result;
    result.reserve(by_channel.size());
    for (auto& [ch, cg] : by_channel) {
        std::ranges::sort(cg.blocks, {}, &BlockGains::start_sample);
        result.push_back(std::move(cg));
    }
    return result;
}

[[nodiscard]] uint64_t block_active_length(const ChannelGainInfo& channel, std::size_t block_index) {
    const auto& block = channel.blocks[block_index];
    uint64_t active_end = block.end_sample;
    if (block_index + 1 < channel.blocks.size()) {
        active_end = std::min(active_end, channel.blocks[block_index + 1].start_sample);
    }
    if (active_end <= block.start_sample) {
        return 0;
    }
    return active_end - block.start_sample;
}

[[nodiscard]] uint64_t
interpolation_length(const ChannelGainInfo& channel, std::size_t block_index, uint64_t default_interp) {
    const auto& block = channel.blocks[block_index];
    if (block.jump_position || block_index == 0) {
        return 0;
    }
    return std::min(block.interp_length_samples.value_or(default_interp), block_active_length(channel, block_index));
}

[[nodiscard]] double interpolated_gain(const std::vector<double>& prev,
                                       const std::vector<double>& cur,
                                       std::size_t out_ch,
                                       uint64_t delta,
                                       uint64_t interp_len) {
    const double alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
    return (prev[out_ch] * (1.0 - alpha)) + (cur[out_ch] * alpha);
}

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
    const uint64_t interp_len = interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;

    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        const double gain =
            ramping ? interpolated_gain(channel.blocks[block_index - 1].gains, block.gains, out_ch, delta, interp_len)
                    : block.gains[out_ch];
        (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += in_sample * static_cast<float>(gain);

        if (ctx.diffuse_in != nullptr) {
            const double diff_gain =
                ramping
                    ? interpolated_gain(
                          channel.blocks[block_index - 1].diffuse_gains, block.diffuse_gains, out_ch, delta, interp_len)
                    : block.diffuse_gains[out_ch];
            ctx.diffuse_in[(frame * ctx.num_out_ch) + out_ch] += static_cast<double>(in_sample) * diff_gain;
        }
    }
}

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now) {
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& channel = gain_matrix[ci];
        if (channel.blocks.empty()) {
            continue;
        }
        for (std::size_t frame = 0; frame < frames_now; ++frame) {
            accumulate_channel_block(channel, block_indices[ci], ctx, frame);
        }
    }
}

// Apply 512-tap FIR decorrelator to each output channel's diffuse input signal.
// diffuse_in:  [frames_now × num_out_ch] planar-interleaved, double
// diffuse_out: [frames_now × num_out_ch] planar-interleaved, float  (written)
void apply_decorrelator(DecorrState& state,
                        const std::vector<double>& diffuse_in,
                        std::vector<float>& diffuse_out,
                        std::size_t frames_now,
                        std::size_t num_out_ch) {
    constexpr std::size_t k_fir_len = 512;
    constexpr std::size_t k_hist_len = k_fir_len - 1; // 511

    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        const auto& fir = state.filters[ch];
        auto& hist = state.fir_hist[ch]; // [511]

        // Build [hist | new samples] for causal convolution.
        std::vector<double> ext(k_hist_len + frames_now);
        std::ranges::copy(hist, ext.begin());
        for (std::size_t f = 0; f < frames_now; ++f) {
            ext[k_hist_len + f] = diffuse_in[(f * num_out_ch) + ch];
        }

        // Direct-form FIR: y[n] = Σ fir[k] × x[n−k]
        for (std::size_t f = 0; f < frames_now; ++f) {
            double y = 0.0;
            for (std::size_t k = 0; k < k_fir_len; ++k) {
                y += fir[k] * ext[k_hist_len + f - k];
            }
            diffuse_out[(f * num_out_ch) + ch] = static_cast<float>(y);
        }

        // Update history: take the last kHistLen elements of ext, which is always
        // valid regardless of frames_now (avoids size_t underflow on short tail blocks).
        for (std::size_t i = 0; i < k_hist_len; ++i) {
            hist[i] = ext[frames_now + i];
        }
    }
}

// Delay the direct bus by comp_delay samples using a circular history buffer.
// Operates in-place on direct_block [frames_now × num_out_ch].
void apply_direct_delay(DecorrState& state,
                        std::vector<float>& direct_block,
                        std::size_t frames_now,
                        std::size_t num_out_ch) {
    const auto delay = static_cast<std::size_t>(state.comp_delay); // 255

    for (std::size_t ch = 0; ch < num_out_ch; ++ch) {
        auto& buf = state.dir_delay[ch]; // [delay]

        // Snapshot the new samples before in-place modification.
        std::vector<float> new_in(frames_now);
        for (std::size_t f = 0; f < frames_now; ++f) {
            new_in[f] = direct_block[(f * num_out_ch) + ch];
        }

        // Output: up to D samples from the delay buffer, then new_in offset by D.
        // Works for any frames_now, including short tail blocks < D.
        const std::size_t from_buf = std::min(delay, frames_now);
        for (std::size_t f = 0; f < from_buf; ++f) {
            direct_block[(f * num_out_ch) + ch] = buf[f];
        }
        for (std::size_t f = from_buf; f < frames_now; ++f) {
            direct_block[(f * num_out_ch) + ch] = new_in[f - delay];
        }

        // Update delay buffer: evict consumed samples, append new_in.
        if (frames_now >= delay) {
            std::ranges::copy(new_in.end() - static_cast<std::ptrdiff_t>(delay), new_in.end(), buf.begin());
        } else {
            // Shift remaining delay left by frames_now, then append new_in at end.
            std::ranges::copy(buf.begin() + static_cast<std::ptrdiff_t>(frames_now), buf.end(), buf.begin());
            std::ranges::copy(new_in, buf.end() - static_cast<std::ptrdiff_t>(frames_now));
        }
    }
}

class EarRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<void> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport EarRenderer::capabilities() const {
    return ear_capabilities();
}

Result<void> EarRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    try {
        const auto& info = plan.scene.info;

        const ear::Layout layout = ear::getLayout(plan.output_layout);
        const auto gain_matrix = build_gain_matrix(plan.scene, layout, logs);

        if (gain_matrix.empty()) {
            logs.log(LogLevel::warning, "ear", "no renderable tracks found (all muted?), writing silence");
        }

        const auto num_out_ch = static_cast<uint16_t>(layout.channels().size());
        const auto num_in_ch = info.num_channels;
        const auto num_frames = info.num_frames;
        const auto sample_rate = info.sample_rate;

        const auto invalid_channel =
            std::ranges::find_if(gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
        if (invalid_channel != gain_matrix.end()) {
            return make_error(ErrorCode::render_failed,
                              fmt::format("track channel index {} is outside input channel count {}",
                                          invalid_channel->input_channel,
                                          num_in_ch),
                              "input=" + plan.input_path);
        }

        logs.log(
            LogLevel::info,
            "ear",
            fmt::format("rendering {} tracks → {} channels, {} frames", gain_matrix.size(), num_out_ch, num_frames));

        progress.on_progress({RenderStage::rendering, 0.3, "rendering audio"});

        // Initialise decorrelator state for the diffuse bus.
        DecorrState decorr;
        decorr.filters = ear::designDecorrelators<double>(layout);
        decorr.comp_delay = ear::decorrelatorCompensationDelay(); // 255
        decorr.fir_hist.assign(num_out_ch, std::vector<double>(511, 0.0));
        decorr.dir_delay.assign(num_out_ch, std::vector<float>(static_cast<std::size_t>(decorr.comp_delay), 0.0F));

        // Open file for audio only — ADM metadata comes from plan.scene.
        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::FloatWavWriter::open(plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate));
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;
        std::vector<std::size_t> blk_idx(gain_matrix.size(), 0);

        constexpr uint64_t k_block_size = 1024;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(num_out_ch) * k_block_size);
        std::vector<double> diffuse_in(static_cast<std::size_t>(num_out_ch) * k_block_size);
        std::vector<float> diffuse_out(static_cast<std::size_t>(num_out_ch) * k_block_size);
        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);
            std::fill(diffuse_in.begin(), diffuse_in.begin() + static_cast<ptrdiff_t>(out_samples), 0.0);
            std::fill(diffuse_out.begin(), diffuse_out.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            // Accumulate direct and diffuse gains into their respective buffers.
            const AccumulateContext ctx{
                in_block.data(), &out_block, diffuse_in.data(), frames_done, num_in_ch, num_out_ch, k_default_interp};
            accumulate_gain_matrix(gain_matrix, blk_idx, ctx, frames_now);

            // Apply 512-tap FIR decorrelator to the diffuse input per channel.
            apply_decorrelator(decorr, diffuse_in, diffuse_out, frames_now, num_out_ch);

            // Delay the direct bus by comp_delay samples to align with diffuse.
            apply_direct_delay(decorr, out_block, frames_now, num_out_ch);

            // Mix delayed direct + decorrelated diffuse.
            for (std::size_t s = 0; s < out_samples; ++s) {
                out_block[s] += diffuse_out[s];
            }

            if (writer.write(out_block.data(), frames_now) != frames_now) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
            }
            frames_done += frames_now;

            const double frac = 0.3 + (0.6 * (static_cast<double>(frames_done) / static_cast<double>(num_frames)));
            progress.on_progress({RenderStage::rendering, frac, "rendering"});
        }

        progress.on_progress({RenderStage::finished, 1.0, "done"});
        logs.log(LogLevel::info, "ear", fmt::format("wrote {} frames to {}", num_frames, plan.output_path));

        return {};

    } catch (const std::invalid_argument& e) {
        // libear throws std::invalid_argument for unknown layout names
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported output layout '{}': {}", plan.output_layout, e.what()),
                          "layout=" + plan.output_layout);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("render failed: ") + e.what(), "input=" + plan.input_path);
    }
}

} // namespace

CapabilityReport ear_capabilities() {
    CapabilityReport r;
    r.backend_name = "libear";
    r.backend_version = "0.9.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = true; // HOA block decode via GainCalculatorHOA
    // clang-format off
    r.supported_layouts = {
        {"0+2+0",  "Stereo",        2,  false, 0, true},
        {"0+5+0",  "5.1",           6,  false, 1, true},
        {"2+5+0",  "5.1.2",         8,  true,  1, true},
        {"4+5+0",  "5.1.4",         10, true,  1, true},
        {"4+5+4",  "9.1.4",         14, true,  1, true},
        {"0+7+0",  "7.1",           8,  false, 1, true},
        {"4+7+0",  "7.1.4",         12, true,  1, true},
        {"9+10+3", "22.2",          24, true,  2, true},
    };
    // clang-format on
    return r;
}

std::unique_ptr<IRenderer> create_ear_renderer() {
    return std::make_unique<EarRenderer>();
}

} // namespace mradm
