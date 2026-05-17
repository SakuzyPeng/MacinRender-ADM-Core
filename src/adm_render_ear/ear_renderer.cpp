#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include <adm/adm.hpp>
#include <adm/parse.hpp>
#include <bw64/bw64.hpp>
#include <ear/ear.hpp>
#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_ear.h"

namespace mradm {

namespace {

std::string axml_to_string(const bw64::AxmlChunk& chunk) {
    std::ostringstream buf;
    chunk.write(buf);
    return buf.str();
}

std::string trim_uid(std::string raw) {
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\0')) {
        raw.pop_back();
    }
    return raw;
}

std::map<std::string, uint16_t> make_uid_to_channel(const std::shared_ptr<bw64::ChnaChunk>& chna) {
    std::map<std::string, uint16_t> result;
    if (!chna) {
        return result;
    }
    for (const auto& entry : chna->audioIds()) {
        std::string uid = trim_uid(entry.uid());
        if (!uid.empty()) {
            result[std::move(uid)] = static_cast<uint16_t>(entry.trackIndex() - 1);
        }
    }
    return result;
}

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<double> direct_gains;
};

// Build a static gain matrix. M3 scope: Objects tracks only, first block per channel.
std::vector<ChannelGainInfo> build_gain_matrix(const std::shared_ptr<adm::Document>& doc,
                                               const std::map<std::string, uint16_t>& uid_to_channel,
                                               const ear::Layout& layout) {
    std::vector<ChannelGainInfo> result;
    ear::GainCalculatorObjects calc{layout};
    const std::size_t num_out = layout.channels().size();

    for (const auto& obj : doc->getElements<adm::AudioObject>()) {
        for (const auto& uid : obj->getReferences<adm::AudioTrackUid>()) {
            const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
            const auto chna_it = uid_to_channel.find(uid_str);
            if (chna_it == uid_to_channel.end()) {
                continue;
            }
            const uint16_t in_ch = chna_it->second;

            const auto pf = uid->getReference<adm::AudioPackFormat>();
            if (!pf) {
                continue;
            }

            for (const auto& cf : pf->getReferences<adm::AudioChannelFormat>()) {
                const auto blocks = cf->getElements<adm::AudioBlockFormatObjects>();
                if (blocks.empty()) {
                    continue;
                }

                const auto& block = blocks.front();
                ear::ObjectsTypeMetadata meta;

                if (block.has<adm::CartesianPosition>()) {
                    const auto& pos = block.get<adm::CartesianPosition>();
                    meta.position = ear::CartesianPosition{
                        static_cast<double>(pos.get<adm::X>().get()),
                        static_cast<double>(pos.get<adm::Y>().get()),
                        static_cast<double>(pos.get<adm::Z>().get()),
                    };
                    meta.cartesian = true;
                } else if (block.has<adm::SphericalPosition>()) {
                    const auto& pos = block.get<adm::SphericalPosition>();
                    double dist = 1.0;
                    if (pos.has<adm::Distance>()) {
                        dist = static_cast<double>(pos.get<adm::Distance>().get());
                    }
                    meta.position = ear::PolarPosition{
                        static_cast<double>(pos.get<adm::Azimuth>().get()),
                        static_cast<double>(pos.get<adm::Elevation>().get()),
                        dist,
                    };
                    meta.cartesian = false;
                } else {
                    continue;
                }

                if (block.has<adm::Gain>()) {
                    meta.gain = block.get<adm::Gain>().get();
                }
                if (block.has<adm::Diffuse>()) {
                    meta.diffuse = static_cast<double>(block.get<adm::Diffuse>().get());
                }
                if (block.has<adm::Width>()) {
                    meta.width = static_cast<double>(block.get<adm::Width>().get());
                }
                if (block.has<adm::Height>()) {
                    meta.height = static_cast<double>(block.get<adm::Height>().get());
                }
                if (block.has<adm::Depth>()) {
                    meta.depth = static_cast<double>(block.get<adm::Depth>().get());
                }

                ChannelGainInfo info;
                info.input_channel = in_ch;
                info.direct_gains.resize(num_out, 0.0);
                std::vector<double> diffuse_gains(num_out, 0.0);
                calc.calculate(meta, info.direct_gains, diffuse_gains);

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
        progress.on_progress({RenderStage::importing_scene, 0.1, "opening input"});
        auto reader = bw64::readFile(plan.input_path);

        const auto axml = reader->axmlChunk();
        if (!axml || axml->size() == 0) {
            return make_error(ErrorCode::io_error, "AXML chunk missing or empty", "input=" + plan.input_path);
        }

        const std::string xml_str = axml_to_string(*axml);
        std::istringstream xml_stream(xml_str);
        const auto doc = adm::parseXml(xml_stream);

        const auto uid_to_channel = make_uid_to_channel(reader->chnaChunk());

        progress.on_progress({RenderStage::planning, 0.2, "building gain matrix"});

        const ear::Layout layout = ear::getLayout(layout_id);
        const auto gain_matrix = build_gain_matrix(doc, uid_to_channel, layout);

        if (gain_matrix.empty()) {
            return make_error(ErrorCode::render_failed,
                              "no renderable Objects tracks found in ADM document",
                              "input=" + plan.input_path);
        }

        const auto num_out_ch = static_cast<uint16_t>(layout.channels().size());
        const auto num_in_ch = reader->channels();
        const auto sample_rate = reader->sampleRate();
        const auto num_frames = reader->numberOfFrames();

        if (sample_rate > std::numeric_limits<uint16_t>::max()) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("sample rate {} Hz is not supported by the current BW64 writer", sample_rate),
                              "input=" + plan.input_path);
        }

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

        // libbw64 writeFile takes uint16_t for sampleRate; cast explicitly.
        auto writer = bw64::writeFile(plan.output_path, num_out_ch, static_cast<uint16_t>(sample_rate), uint16_t{24});

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
                    const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                    for (std::size_t out_ch = 0; out_ch < num_out_ch; out_ch++) {
                        out_block[(f * num_out_ch) + out_ch] += in_s * static_cast<float>(cg.direct_gains[out_ch]);
                    }
                }
            }

            writer->write(out_block.data(), frames_now);
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
    r.supports_direct_speakers = false;
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
