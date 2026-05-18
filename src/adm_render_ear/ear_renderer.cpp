#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include <bw64/bw64.hpp>
#include <ear/ear.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_ear.h"

namespace mradm {

namespace {

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<double> direct_gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
};

// Build a static gain matrix from pre-parsed scene metadata.
// Uses imported block metadata from each track; M6 scope: Objects + DirectSpeakers.
std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene, const ear::Layout& layout) {
    std::vector<ChannelGainInfo> result;
    ear::GainCalculatorObjects objects_calc{layout};
    ear::GainCalculatorDirectSpeakers direct_speakers_calc{layout};
    const std::size_t num_out = layout.channels().size();

    for (const auto& obj : scene.objects) {
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = *track.channel_index;

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

                ChannelGainInfo info;
                info.input_channel = in_ch;
                info.direct_gains.resize(num_out, 0.0);
                info.start_sample = block.start_sample;
                info.end_sample = block.end_sample;
                std::vector<double> diffuse_gains(num_out, 0.0);
                objects_calc.calculate(meta, info.direct_gains, diffuse_gains);

                result.push_back(std::move(info));
            }

            for (const auto& block : track.ds_blocks) {
                ear::DirectSpeakersTypeMetadata meta;
                meta.speakerLabels = block.speaker_labels;
                if (!block.pack_format_id.empty()) {
                    meta.audioPackFormatID = block.pack_format_id;
                }
                if (block.has_position) {
                    meta.position = ear::PolarSpeakerPosition{
                        static_cast<double>(block.azimuth),
                        static_cast<double>(block.elevation),
                        static_cast<double>(block.distance),
                    };
                }

                ChannelGainInfo info;
                info.input_channel = in_ch;
                info.direct_gains.resize(num_out, 0.0);
                direct_speakers_calc.calculate(meta, info.direct_gains);

                const auto block_gain = static_cast<double>(block.gain);
                std::ranges::transform(info.direct_gains, info.direct_gains.begin(), [block_gain](double gain) {
                    return gain * block_gain;
                });

                result.push_back(std::move(info));
            }
        }
    }

    return result;
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

        // Open file for audio only — ADM metadata comes from plan.scene.
        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::FloatWavWriter::open(plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate));
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_block_size = 1024;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(num_out_ch) * k_block_size);
        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            for (const auto& cg : gain_matrix) {
                for (std::size_t f = 0; f < frames_now; f++) {
                    const uint64_t abs_frame = frames_done + f;
                    if (abs_frame < cg.start_sample || abs_frame >= cg.end_sample) {
                        continue;
                    }
                    const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                    for (std::size_t out_ch = 0; out_ch < num_out_ch; out_ch++) {
                        out_block[(f * num_out_ch) + out_ch] += in_s * static_cast<float>(cg.direct_gains[out_ch]);
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
