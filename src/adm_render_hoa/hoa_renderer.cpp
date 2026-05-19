#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
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
    constexpr float sqrt3 = std::numbers::sqrt3_v<float>;
    const float sqrt15 = std::sqrt(15.0F);
    const float sqrt5_8 = std::sqrt(5.0F / 8.0F);
    const float sqrt3_8 = std::sqrt(3.0F / 8.0F);

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

struct HoaBlock {
    Hoa3Coeffs gains{};
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<HoaBlock> blocks; // sorted ascending by start_sample
};

// Returns linearly interpolated HOA gains at abs_frame for the given channel.
// Returns all-zero coefficients when abs_frame is outside every block.
[[nodiscard]] Hoa3Coeffs gains_at(const ChannelGainInfo& cg, uint64_t abs_frame, uint64_t default_interp) {
    // Upper-bound search: find the first block whose start_sample > abs_frame.
    const auto it = std::ranges::upper_bound(cg.blocks, abs_frame, {}, &HoaBlock::start_sample);
    if (it == cg.blocks.begin()) {
        return {};
    }
    const auto cur_it = std::prev(it); // cur_it->start_sample <= abs_frame
    const HoaBlock& cur = *cur_it;
    if (abs_frame >= cur.end_sample) {
        return {}; // frame is past this block's end
    }
    // Interpolation ramp: blend from previous block gains when jump_position is false.
    if (!cur.jump_position && cur_it != cg.blocks.begin()) {
        const HoaBlock& prev = *std::prev(cur_it);
        const uint64_t interp_len = cur.interp_length_samples.value_or(default_interp);
        const uint64_t delta = abs_frame - cur.start_sample;
        if (interp_len > 0 && delta < interp_len) {
            const double alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
            Hoa3Coeffs result;
            for (std::size_t i = 0; i < k_hoa3_channels; ++i) {
                result[i] = static_cast<float>(static_cast<double>(prev.gains[i]) * (1.0 - alpha) +
                                               static_cast<double>(cur.gains[i]) * alpha);
            }
            return result;
        }
    }
    return cur.gains;
}

std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene) {
    std::map<uint16_t, ChannelGainInfo> by_channel;

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

            for (const auto& block : track.blocks) {
                const SceneBlockPosition pos =
                    obj.position_offset ? apply_position_offset(block.position, *obj.position_offset) : block.position;
                Hoa3Coeffs sh =
                    pos.cartesian ? encode_cartesian(pos.x, pos.y, pos.z) : encode_polar(pos.azimuth, pos.elevation);
                const float combined_gain = block.gain * obj.gain;
                std::ranges::transform(sh, sh.begin(), [combined_gain](float c) { return c * combined_gain; });
                cg.blocks.push_back({sh,
                                     block.start_sample,
                                     std::min(block.end_sample, obj.end_sample),
                                     block.jump_position,
                                     block.interp_length_samples});
            }
        }
    }

    std::vector<ChannelGainInfo> result;
    result.reserve(by_channel.size());
    for (auto& [ch, cg] : by_channel) {
        std::ranges::sort(cg.blocks, {}, &HoaBlock::start_sample);
        result.push_back(std::move(cg));
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

    auto gain_matrix = build_gain_matrix(plan.scene);
    if (gain_matrix.empty()) {
        return make_error(
            ErrorCode::render_failed, "no renderable Objects tracks found in ADM document", "input=" + plan.input_path);
    }

    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;
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
        logs.log(
            LogLevel::info,
            "hoa-encode",
            fmt::format(
                "encoding {} Objects tracks → HOA3 ({} ch), {} frames", gain_matrix.size(), k_num_out, num_frames));
        progress.on_progress({RenderStage::rendering, 0.3, "encoding HOA"});

        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::FloatWavWriter::open(plan.output_path, k_num_out, static_cast<uint32_t>(sample_rate));
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_block_size = 1024;
        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * 5 / 1000; // 5 ms
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
                    const uint64_t abs_frame = frames_done + f;
                    const Hoa3Coeffs gains = gains_at(cg, abs_frame, k_default_interp);
                    const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                    std::size_t out_index = f * k_num_out;
                    for (const float gain : gains) {
                        out_block[out_index] += in_s * gain;
                        ++out_index;
                    }
                }
            }

            if (writer.write(out_block.data(), frames_now) != frames_now) {
                return make_error(ErrorCode::io_error, "short write while encoding HOA", "output=" + plan.output_path);
            }
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
