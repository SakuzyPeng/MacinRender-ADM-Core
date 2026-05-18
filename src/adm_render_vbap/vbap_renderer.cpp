#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <saf_vbap.h>
#include <string_view>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_vbap.h"

namespace mradm {

namespace {

struct SpeakerDirection {
    float azimuth{0.0F};
    float elevation{0.0F};
};

struct LayoutSpec {
    std::vector<SpeakerDirection> speakers;
};

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<float> gains;
};

struct SafFree {
    // SAF allocates gain tables through its C API; ownership transfers to caller.
    void operator()(float* ptr) const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        std::free(ptr);
    }
};

[[nodiscard]] std::optional<LayoutSpec> layout_spec(std::string_view layout_id) {
    if (layout_id == "0+2+0") {
        return LayoutSpec{{{-30.0F, 0.0F}, {30.0F, 0.0F}}};
    }
    if (layout_id == "0+5+0") {
        return LayoutSpec{{{-30.0F, 0.0F}, {30.0F, 0.0F}, {0.0F, 0.0F}, {-110.0F, 0.0F}, {110.0F, 0.0F}}};
    }
    if (layout_id == "0+7+0") {
        return LayoutSpec{{{-30.0F, 0.0F},
                           {30.0F, 0.0F},
                           {0.0F, 0.0F},
                           {-90.0F, 0.0F},
                           {90.0F, 0.0F},
                           {-150.0F, 0.0F},
                           {150.0F, 0.0F}}};
    }
    if (layout_id == "4+5+0") {
        return LayoutSpec{{{-30.0F, 0.0F},
                           {30.0F, 0.0F},
                           {0.0F, 0.0F},
                           {-110.0F, 0.0F},
                           {110.0F, 0.0F},
                           {-45.0F, 45.0F},
                           {45.0F, 45.0F},
                           {-135.0F, 45.0F},
                           {135.0F, 45.0F}}};
    }
    if (layout_id == "4+7+0") {
        return LayoutSpec{{{-30.0F, 0.0F},
                           {30.0F, 0.0F},
                           {0.0F, 0.0F},
                           {-90.0F, 0.0F},
                           {90.0F, 0.0F},
                           {-150.0F, 0.0F},
                           {150.0F, 0.0F},
                           {-45.0F, 45.0F},
                           {45.0F, 45.0F},
                           {-135.0F, 45.0F},
                           {135.0F, 45.0F}}};
    }
    if (layout_id == "9+10+3") {
        return LayoutSpec{{{-30.0F, 0.0F},   {30.0F, 0.0F},    {0.0F, 0.0F},    {-90.0F, 0.0F},   {90.0F, 0.0F},
                           {-150.0F, 0.0F},  {150.0F, 0.0F},   {180.0F, 0.0F},  {-30.0F, 30.0F},  {30.0F, 30.0F},
                           {0.0F, 30.0F},    {-90.0F, 30.0F},  {90.0F, 30.0F},  {-150.0F, 30.0F}, {150.0F, 30.0F},
                           {180.0F, 30.0F},  {-30.0F, -30.0F}, {30.0F, -30.0F}, {0.0F, -30.0F},   {-110.0F, -30.0F},
                           {110.0F, -30.0F}, {180.0F, -30.0F}}};
    }
    return std::nullopt;
}

[[nodiscard]] bool is_2d_layout(const LayoutSpec& layout) {
    return std::ranges::all_of(layout.speakers,
                               [](const auto& speaker) { return std::fabs(speaker.elevation) < 1.0e-6F; });
}

[[nodiscard]] std::vector<float> flatten_layout(const LayoutSpec& layout) {
    std::vector<float> result;
    result.reserve(layout.speakers.size() * 2U);
    for (const auto& speaker : layout.speakers) {
        result.push_back(speaker.azimuth);
        result.push_back(speaker.elevation);
    }
    return result;
}

[[nodiscard]] SpeakerDirection source_direction(const SceneBlockPosition& pos) {
    if (!pos.cartesian) {
        return {pos.azimuth, pos.elevation};
    }

    const auto x = static_cast<double>(pos.x);
    const auto y = static_cast<double>(pos.y);
    const auto z = static_cast<double>(pos.z);
    const auto xy = std::hypot(x, y);
    if (xy == 0.0 && z == 0.0) {
        return {};
    }

    constexpr auto rad_to_deg = 180.0 / std::numbers::pi;
    return {static_cast<float>(std::atan2(-x, y) * rad_to_deg), static_cast<float>(std::atan2(z, xy) * rad_to_deg)};
}

// Map ADM Objects extent parameters to a MDAP spread angle in degrees.
// Port of ADMVBAPMDAPSpreadDegreesForExtent from the ObjC renderer.
// 2D layouts pass spread=0 to the SAF API (2D VBAP has no spread parameter).
[[nodiscard]] float mdap_spread_degrees(const SceneObjectBlock& block) {
    const float distance = block.position.cartesian ? std::hypot(block.position.x, block.position.y, block.position.z)
                                                    : block.position.distance;
    const float spread_scale = std::clamp(1.0F / std::max(0.4F, distance), 0.5F, 2.5F);
    const float w = std::max(0.0F, block.width) * 60.0F * spread_scale;
    const float h = std::max(0.0F, block.height) * 45.0F * spread_scale;
    const float d = std::max(0.0F, block.depth) * 20.0F * spread_scale;
    return std::min(180.0F, std::hypot(w, h, d));
}

[[nodiscard]] Result<std::vector<float>> calculate_vbap_gains(const SceneObjectBlock& block, const LayoutSpec& layout) {
    auto speakers = flatten_layout(layout);
    const auto src = source_direction(block.position);
    std::vector<float> source{src.azimuth, src.elevation};

    const auto speaker_count = static_cast<int>(layout.speakers.size());
    const bool use_3d = !is_2d_layout(layout);
    const float spread_deg = use_3d ? mdap_spread_degrees(block) : 0.0F;
    int table_size = 0;
    int simplex_count = 0;
    float* raw_table = nullptr;

    if (!use_3d) {
        generateVBAPgainTable2D_srcs(
            source.data(), 1, speakers.data(), speaker_count, &raw_table, &table_size, &simplex_count);
    } else {
        constexpr int k_omit_large_triangles = 1;
        constexpr int k_enable_dummies = 1;
        generateVBAPgainTable3D_srcs(source.data(),
                                     1,
                                     speakers.data(),
                                     speaker_count,
                                     k_omit_large_triangles,
                                     k_enable_dummies,
                                     spread_deg,
                                     &raw_table,
                                     &table_size,
                                     &simplex_count);
    }

    std::unique_ptr<float, SafFree> table{raw_table};
    if (table == nullptr || table_size != 1) {
        return make_error(ErrorCode::render_failed, "SAF VBAP gain calculation failed", {});
    }

    std::vector<float> gains(static_cast<std::size_t>(speaker_count), 0.0F);
    std::copy_n(table.get(), gains.size(), gains.begin());
    std::ranges::transform(gains, gains.begin(), [block_gain = block.gain](float gain) { return gain * block_gain; });
    return gains;
}

[[nodiscard]] Result<std::vector<ChannelGainInfo>> build_gain_matrix(const AdmScene& scene, const LayoutSpec& layout) {
    std::vector<ChannelGainInfo> result;

    for (const auto& obj : scene.objects) {
        for (const auto& track : obj.tracks) {
            const auto channel_index = track.channel_index;
            if (!channel_index.has_value()) {
                continue;
            }
            for (const auto& block : track.blocks) {
                auto gains = calculate_vbap_gains(block, layout);
                if (!gains) {
                    return make_error(
                        gains.error().code, gains.error().message, fmt::format("track_uid={}", track.track_uid));
                }

                ChannelGainInfo info;
                info.input_channel = channel_index.value();
                info.gains = std::move(*gains);
                result.push_back(std::move(info));
            }
        }
    }

    return result;
}

class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<void> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

Result<void> VbapRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    std::string layout_id = plan.output_layout;
    if (layout_id == "binaural") {
        layout_id = "0+2+0";
    }

    const auto layout = layout_spec(layout_id);
    if (!layout.has_value()) {
        return make_error(ErrorCode::unsupported, fmt::format("unsupported VBAP output layout '{}'", layout_id), {});
    }

    const auto& info = plan.scene.info;
    if (info.sample_rate > std::numeric_limits<uint16_t>::max()) {
        return make_error(
            ErrorCode::unsupported,
            fmt::format("sample rate {} Hz is not supported by the current BW64 writer", info.sample_rate),
            "input=" + plan.input_path);
    }

    auto gain_matrix = build_gain_matrix(plan.scene, *layout);
    if (!gain_matrix) {
        return make_error(gain_matrix.error().code, gain_matrix.error().message, gain_matrix.error().context);
    }
    if (gain_matrix->empty()) {
        return make_error(
            ErrorCode::render_failed, "no renderable Objects tracks found in ADM document", "input=" + plan.input_path);
    }

    const auto num_out_ch = static_cast<uint16_t>(layout->speakers.size());
    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = static_cast<uint16_t>(info.sample_rate);

    const auto invalid_channel =
        std::ranges::find_if(*gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
    if (invalid_channel != gain_matrix->end()) {
        return make_error(ErrorCode::render_failed,
                          fmt::format("track channel index {} is outside input channel count {}",
                                      invalid_channel->input_channel,
                                      num_in_ch),
                          "input=" + plan.input_path);
    }

    try {
        logs.log(
            LogLevel::info,
            "saf-vbap",
            fmt::format(
                "rendering {} Objects tracks → {} channels, {} frames", gain_matrix->size(), num_out_ch, num_frames));
        progress.on_progress({RenderStage::rendering, 0.3, "rendering audio"});

        auto reader = bw64::readFile(plan.input_path);
        auto writer = bw64::writeFile(plan.output_path, num_out_ch, sample_rate, uint16_t{24});

        constexpr uint64_t k_block_size = 1024;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(num_out_ch) * k_block_size);
        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            for (const auto& cg : *gain_matrix) {
                for (std::size_t frame = 0; frame < frames_now; frame++) {
                    const float in_sample = in_block[(frame * num_in_ch) + cg.input_channel];
                    for (std::size_t out_ch = 0; out_ch < num_out_ch; out_ch++) {
                        out_block[(frame * num_out_ch) + out_ch] += in_sample * cg.gains[out_ch];
                    }
                }
            }

            writer->write(out_block.data(), frames_now);
            frames_done += frames_now;

            const double frac = 0.3 + (0.6 * (static_cast<double>(frames_done) / static_cast<double>(num_frames)));
            progress.on_progress({RenderStage::rendering, frac, "rendering"});
        }

        progress.on_progress({RenderStage::finished, 1.0, "done"});
        logs.log(LogLevel::info, "saf-vbap", fmt::format("wrote {} frames to {}", num_frames, plan.output_path));

        return {};
    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::io_error, std::string("VBAP render failed: ") + e.what(), "input=" + plan.input_path);
    }
}

} // namespace

CapabilityReport vbap_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-vbap";
    r.backend_version = "1.3.4";
    r.supports_objects = true;
    r.supports_direct_speakers = false;
    r.supports_hoa = false;
    r.supported_layouts = {
        {"0+2+0", "Stereo"},
        {"0+5+0", "5.0"},
        {"0+7+0", "7.0"},
        {"4+5+0", "5.1.4"},
        {"4+7+0", "7.1.4"},
        {"9+10+3", "9.1.6"},
    };
    return r;
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
