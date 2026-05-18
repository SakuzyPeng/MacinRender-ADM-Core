#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/render.h"
#include "adm/render_hoa.h"

namespace mradm {

namespace {

constexpr std::size_t k_hoa3_channels = 16; // (3+1)^2 = 4^2

constexpr uint16_t k_hoa3_channels_u16 = static_cast<uint16_t>(k_hoa3_channels);
using Hoa3Coeffs = std::array<float, k_hoa3_channels>;

// Compute real SN3D spherical harmonic coefficients for order 3 (ACN channel
// ordering), given a Cartesian unit direction in HOA convention (X=front,
// Y=left, Z=up).
//
// Matches ADMHOAEncoder::encodeOrder3SN3DForDirectionX:y:z: from the ObjC
// renderer. Callers must ensure the vector is already normalised.
Hoa3Coeffs sh_sn3d_3(float x, float y, float z) noexcept {
    const float sqrt3    = std::sqrt(3.0F);
    const float sqrt15   = std::sqrt(15.0F);
    const float sqrt5_8  = std::sqrt(5.0F / 8.0F);
    const float sqrt3_8  = std::sqrt(3.0F / 8.0F);

    return {
        // n=0
        1.0F,
        // n=1
        y,
        z,
        x,
        // n=2
        sqrt3 * x * y,
        sqrt3 * y * z,
        0.5F * (3.0F * z * z - 1.0F),
        sqrt3 * x * z,
        0.5F * sqrt3 * (x * x - y * y),
        // n=3
        sqrt5_8 * y * (3.0F * x * x - y * y),
        sqrt15 * x * y * z,
        sqrt3_8 * y * (5.0F * z * z - 1.0F),
        0.5F * z * (5.0F * z * z - 3.0F),
        sqrt3_8 * x * (5.0F * z * z - 1.0F),
        0.5F * sqrt15 * z * (x * x - y * y),
        sqrt5_8 * x * (x * x - 3.0F * y * y),
    };
}

// ADM polar (standard convention: az=0→front, +az→left CCW) to HOA Cartesian
// (X=front, Y=left, Z=up), then encode.
Hoa3Coeffs encode_polar(float az_deg, float el_deg) noexcept {
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const float az = az_deg * k_deg2rad;
    const float el = el_deg * k_deg2rad;
    const float cos_el = std::cos(el);
    // Standard ADM: +az = left (CCW) → sin(az) gives positive Y for left sources.
    return sh_sn3d_3(cos_el * std::cos(az), cos_el * std::sin(az), std::sin(el));
}

// ADM Cartesian (X=right, Y=front, Z=up) → HOA (X=front, Y=left, Z=up).
Hoa3Coeffs encode_cartesian(float xc, float yc, float zc) noexcept {
    const float len = std::max(1.0e-6F, std::hypot(xc, yc, zc));
    return sh_sn3d_3(yc / len, -xc / len, zc / len);
}

struct ChannelGainInfo {
    uint16_t input_channel{0};
    Hoa3Coeffs gains{};
};

std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene) {
    std::vector<ChannelGainInfo> result;

    for (const auto& obj : scene.objects) {
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = *track.channel_index;

            for (const auto& block : track.blocks) {
                Hoa3Coeffs sh = block.position.cartesian
                    ? encode_cartesian(block.position.x, block.position.y, block.position.z)
                    : encode_polar(block.position.azimuth, block.position.elevation);

                for (auto& c : sh) {
                    c *= block.gain;
                }

                result.push_back({in_ch, sh});
            }
        }
    }

    return result;
}

class HoaRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<void> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport HoaRenderer::capabilities() const {
    return hoa_capabilities();
}

Result<void> HoaRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    if (plan.output_layout != "hoa3") {
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported HOA output layout '{}'; supported: hoa3", plan.output_layout),
                          {});
    }

    const auto& info = plan.scene.info;
    if (info.sample_rate > std::numeric_limits<uint16_t>::max()) {
        return make_error(
            ErrorCode::unsupported,
            fmt::format("sample rate {} Hz is not supported by the current BW64 writer", info.sample_rate),
            "input=" + plan.input_path);
    }

    auto gain_matrix = build_gain_matrix(plan.scene);
    if (gain_matrix.empty()) {
        return make_error(
            ErrorCode::render_failed, "no renderable Objects tracks found in ADM document", "input=" + plan.input_path);
    }

    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = static_cast<uint16_t>(info.sample_rate);
    constexpr uint16_t k_num_out = k_hoa3_channels_u16;

    const auto invalid_channel =
        std::ranges::find_if(gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
    if (invalid_channel != gain_matrix.end()) {
        return make_error(ErrorCode::render_failed,
                          fmt::format("track channel index {} is outside input channel count {}",
                                      invalid_channel->input_channel,
                                      num_in_ch),
                          "input=" + plan.input_path);
    }

    try {
        logs.log(LogLevel::info,
                 "hoa-encode",
                 fmt::format("encoding {} Objects tracks → HOA3 ({} ch), {} frames",
                             gain_matrix.size(),
                             k_num_out,
                             num_frames));
        progress.on_progress({RenderStage::rendering, 0.3, "encoding HOA"});

        auto reader = bw64::readFile(plan.input_path);
        auto writer = bw64::writeFile(plan.output_path, k_num_out, sample_rate, uint16_t{24});

        constexpr uint64_t k_block_size = 1024;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(k_num_out) * k_block_size);
        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(k_num_out) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            for (const auto& cg : gain_matrix) {
                for (std::size_t f = 0; f < frames_now; ++f) {
                    const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                    for (std::size_t ch = 0; ch < k_num_out; ++ch) {
                        out_block[(f * k_num_out) + ch] += in_s * cg.gains[ch];
                    }
                }
            }

            writer->write(out_block.data(), frames_now);
            frames_done += frames_now;

            const double frac = 0.3 + (0.6 * (static_cast<double>(frames_done) / static_cast<double>(num_frames)));
            progress.on_progress({RenderStage::rendering, frac, "encoding"});
        }

        progress.on_progress({RenderStage::finished, 1.0, "done"});
        logs.log(LogLevel::info, "hoa-encode", fmt::format("wrote {} frames to {}", num_frames, plan.output_path));

        return {};

    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::io_error, std::string("HOA encode failed: ") + e.what(), "input=" + plan.input_path);
    }
}

} // namespace

CapabilityReport hoa_capabilities() {
    CapabilityReport r;
    r.backend_name = "hoa-encode";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = false;
    r.supports_hoa = false;
    r.supported_layouts = {
        {"hoa3", "HOA 3rd Order (16ch, ACN/SN3D)"},
    };
    return r;
}

std::unique_ptr<IRenderer> create_hoa_renderer() {
    return std::make_unique<HoaRenderer>();
}

} // namespace mradm
