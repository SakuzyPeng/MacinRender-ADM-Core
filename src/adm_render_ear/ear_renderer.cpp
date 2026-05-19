#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
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
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<BlockGains> blocks; // sorted by start_sample
};

std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene, const ear::Layout& layout) {
    std::map<uint16_t, ChannelGainInfo> by_channel;
    ear::GainCalculatorObjects objects_calc{layout};
    ear::GainCalculatorDirectSpeakers direct_speakers_calc{layout};
    const std::size_t num_out = layout.channels().size();

    for (const auto& obj : scene.objects) {
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = *track.channel_index;
            auto& cg = by_channel[in_ch];
            cg.input_channel = in_ch;

            for (const auto& block : track.blocks) {
                ear::ObjectsTypeMetadata meta;

                if (block.position.cartesian) {
                    meta.position = ear::CartesianPosition{
                        static_cast<double>(block.position.x),
                        static_cast<double>(block.position.y),
                        static_cast<double>(block.position.z),
                    };
                    meta.cartesian = true;
                } else {
                    meta.position = ear::PolarPosition{
                        static_cast<double>(block.position.azimuth),
                        static_cast<double>(block.position.elevation),
                        static_cast<double>(block.position.distance),
                    };
                    meta.cartesian = false;
                }

                meta.gain = static_cast<double>(block.gain);
                meta.diffuse = static_cast<double>(block.diffuse);
                meta.width = static_cast<double>(block.width);
                meta.height = static_cast<double>(block.height);
                meta.depth = static_cast<double>(block.depth);

                BlockGains bg;
                bg.gains.resize(num_out, 0.0);
                bg.start_sample = block.start_sample;
                bg.end_sample = block.end_sample;
                bg.jump_position = block.jump_position;
                bg.interp_length_samples = block.interp_length_samples;
                std::vector<double> diffuse_gains(num_out, 0.0);
                objects_calc.calculate(meta, bg.gains, diffuse_gains);
                cg.blocks.push_back(std::move(bg));
            }

            for (const auto& ds : track.ds_blocks) {
                ear::DirectSpeakersTypeMetadata meta;
                meta.speakerLabels = ds.speaker_labels;
                if (!ds.pack_format_id.empty()) {
                    meta.audioPackFormatID = ds.pack_format_id;
                }
                if (ds.has_position) {
                    meta.position = ear::PolarSpeakerPosition{
                        static_cast<double>(ds.azimuth),
                        static_cast<double>(ds.elevation),
                        static_cast<double>(ds.distance),
                    };
                }

                BlockGains bg;
                bg.gains.resize(num_out, 0.0);
                bg.jump_position = true;
                direct_speakers_calc.calculate(meta, bg.gains);
                const auto block_gain = static_cast<double>(ds.gain);
                std::ranges::transform(bg.gains, bg.gains.begin(), [block_gain](double g) { return g * block_gain; });
                cg.blocks.push_back(std::move(bg));
            }
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

[[nodiscard]] uint64_t interpolation_length(const BlockGains& block,
                                            std::size_t block_index,
                                            uint64_t default_interp) {
    if (block.jump_position || block_index == 0) {
        return 0;
    }
    return block.interp_length_samples.value_or(default_interp);
}

[[nodiscard]] double interpolated_gain(const BlockGains& previous,
                                       const BlockGains& current,
                                       std::size_t out_ch,
                                       uint64_t delta,
                                       uint64_t interp_len) {
    const double alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
    return previous.gains[out_ch] * (1.0 - alpha) + current.gains[out_ch] * alpha;
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
    // Map the "binaural" layout alias before any validation.
    std::string layout_id = plan.output_layout;
    if (layout_id == "binaural") {
        layout_id = "0+2+0";
    }

    try {
        const auto& info = plan.scene.info;

        const ear::Layout layout = ear::getLayout(layout_id);
        const auto gain_matrix = build_gain_matrix(plan.scene, layout);

        if (gain_matrix.empty()) {
            return make_error(
                ErrorCode::render_failed, "no renderable tracks found in ADM document", "input=" + plan.input_path);
        }

        const auto num_out_ch = static_cast<uint16_t>(layout.channels().size());
        const auto num_in_ch = info.num_channels;
        const auto num_frames = info.num_frames;
        const auto sample_rate = info.sample_rate;

        const auto invalid_channel = std::ranges::find_if(
            gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
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

        // Open file for audio only — ADM metadata comes from plan.scene.
        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::FloatWavWriter::open(plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate));
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * 5 / 1000;
        std::vector<std::size_t> blk_idx(gain_matrix.size(), 0);

        constexpr uint64_t k_block_size = 1024;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(num_out_ch) * k_block_size);
        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
                const auto& channel = gain_matrix[ci];
                if (channel.blocks.empty()) {
                    continue;
                }
                for (std::size_t f = 0; f < frames_now; ++f) {
                    const uint64_t abs_frame = frames_done + f;

                    while (blk_idx[ci] + 1 < channel.blocks.size() &&
                           abs_frame >= channel.blocks[blk_idx[ci] + 1].start_sample) {
                        ++blk_idx[ci];
                    }

                    const auto& blk = channel.blocks[blk_idx[ci]];
                    if (abs_frame < blk.start_sample || abs_frame >= blk.end_sample) {
                        continue;
                    }

                    const float in_s = in_block[(f * num_in_ch) + channel.input_channel];
                    const uint64_t interp_len = interpolation_length(blk, blk_idx[ci], k_default_interp);
                    const uint64_t delta = abs_frame - blk.start_sample;
                    const bool ramping = interp_len > 0 && delta < interp_len;

                    for (std::size_t out_ch = 0; out_ch < num_out_ch; ++out_ch) {
                        const double gain =
                            ramping ? interpolated_gain(channel.blocks[blk_idx[ci] - 1], blk, out_ch, delta, interp_len)
                                    : blk.gains[out_ch];
                        out_block[(f * num_out_ch) + out_ch] += in_s * static_cast<float>(gain);
                    }
                }
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
                          fmt::format("unsupported output layout '{}': {}", layout_id, e.what()),
                          "layout=" + layout_id);
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
    r.supports_hoa = false;
    r.supported_layouts = {
        {"0+2+0", "Stereo"},
        {"0+5+0", "5.0"},
        {"2+5+0", "5.1+2H"},
        {"4+5+0", "5.1+4H"},
        {"4+5+4", "9.1.4"},
        {"0+7+0", "7.0"},
        {"4+7+0", "7.1+4H"},
        {"9+10+3", "22.2"},
    };
    return r;
}

std::unique_ptr<IRenderer> create_ear_renderer() {
    return std::make_unique<EarRenderer>();
}

} // namespace mradm
