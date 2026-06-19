#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ebur128.h>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
// clang-format off
// saf_utility_complex.h must precede saf_hoa.h: saf_hoa.h opens extern "C" and
// re-includes saf_utility_complex.h inside that block, causing std::complex<float>
// template instantiation in C linkage if the include guard hasn't fired yet.
#include <saf_utility_complex.h>
#include <saf_hoa.h>
// clang-format on

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_hoa.h"

#include "render_common.h"

namespace mradm {

namespace {

#ifdef _MSC_VER
#define MRADM_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define MRADM_RESTRICT __restrict__
#else
#define MRADM_RESTRICT
#endif

constexpr std::size_t k_hoa3_channels = 16; // (3+1)^2 = 4^2
constexpr std::size_t k_diffuse_dirs = 32;
constexpr std::size_t k_diffuse_delay_len = 1024;
constexpr std::size_t k_diffuse_slots = 3; // left / center / right divergence components

constexpr uint16_t k_hoa3_channels_u16 = static_cast<uint16_t>(k_hoa3_channels);
using Hoa3Coeffs = std::array<float, k_hoa3_channels>;
using DiffuseSlots = std::array<Hoa3Coeffs, k_diffuse_slots>;

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] Vec3 normalize(Vec3 v) noexcept {
    const float len = std::max(1.0e-6F, std::hypot(v.x, v.y, v.z));
    return {v.x / len, v.y / len, v.z / len};
}

[[nodiscard]] Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {
        (a.y * b.z) - (a.z * b.y),
        (a.z * b.x) - (a.x * b.z),
        (a.x * b.y) - (a.y * b.x),
    };
}

[[nodiscard]] Vec3 add(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec3 scale(Vec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

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
// (X=front, Y=left, Z=up).
Vec3 direction_from_polar(float az_deg, float el_deg) noexcept {
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const float az = az_deg * k_deg2rad;
    const float el = el_deg * k_deg2rad;
    const float cos_el = std::cos(el);
    // Standard ADM: +az = left (CCW) → sin(az) gives positive Y for left sources.
    return {cos_el * std::cos(az), cos_el * std::sin(az), std::sin(el)};
}

// ADM Cartesian (X=right, Y=front, Z=up) → HOA (X=front, Y=left, Z=up).
Vec3 direction_from_cartesian(float xc, float yc, float zc) noexcept {
    return normalize({yc, -xc, zc});
}

Hoa3Coeffs encode_direction(Vec3 dir) noexcept {
    const Vec3 n = normalize(dir);
    return sh_sn3d_3(n.x, n.y, n.z);
}

Hoa3Coeffs encode_polar(float az_deg, float el_deg) noexcept {
    return encode_direction(direction_from_polar(az_deg, el_deg));
}

[[nodiscard]] Vec3 direction_from_position(const SceneBlockPosition& pos) noexcept {
    return pos.cartesian ? direction_from_cartesian(pos.x, pos.y, pos.z)
                         : direction_from_polar(pos.azimuth, pos.elevation);
}

[[nodiscard]] float distance_from_position(const SceneBlockPosition& pos) noexcept {
    return pos.cartesian ? std::hypot(pos.x, pos.y, pos.z) : pos.distance;
}

[[nodiscard]] Hoa3Coeffs encode_extent(const SceneBlockPosition& pos, const SceneObjectBlock& block) {
    const float distance = distance_from_position(pos);
    const auto [width_radius, height_radius] =
        render_common::extent_disk_radii(block.width, block.height, block.depth, distance);
    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        return encode_direction(direction_from_position(pos));
    }

    // Shared 17-point disk cloud (render_common); the per-backend geometry below
    // (direction_from_position / normalize / encode_direction) stays local and unchanged so
    // the HOA output remains bit-identical.
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const auto& k_samples = render_common::k_extent_disk_samples;

    const Vec3 center = direction_from_position(pos);
    Vec3 horizontal = cross({0.0F, 0.0F, 1.0F}, center);
    if (std::hypot(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
        horizontal = {1.0F, 0.0F, 0.0F};
    } else {
        horizontal = normalize(horizontal);
    }
    const Vec3 vertical = normalize(cross(center, horizontal));

    Hoa3Coeffs result{};
    for (const auto& sample : k_samples) {
        const float h = std::tan(sample.x * width_radius * k_deg2rad);
        const float v = std::tan(sample.y * height_radius * k_deg2rad);
        const Vec3 dir = normalize(add(add(center, scale(horizontal, h)), scale(vertical, v)));
        const Hoa3Coeffs coeffs = encode_direction(dir);
        for (std::size_t i = 0; i < k_hoa3_channels; ++i) {
            result.at(i) += coeffs.at(i) * sample.weight;
        }
    }
    return result;
}

// Parse a BS.2051 speaker label (e.g. "M+030", "U-045", "T+000") into (az, el) degrees.
// Returns nullopt when the label is not a recognised positional format or contains
// trailing non-numeric characters after the azimuth digits.
std::optional<std::pair<float, float>> parse_speaker_label(const std::string& label) {
    if (label.size() < 5) {
        return std::nullopt;
    }
    float el = 0.0F;
    switch (label[0]) {
    case 'M':
        el = 0.0F;
        break;
    case 'U':
        el = 30.0F;
        break;
    case 'T':
        el = 90.0F;
        break;
    case 'B':
        el = -30.0F;
        break;
    default:
        return std::nullopt;
    }
    if (label[1] != '+' && label[1] != '-') {
        return std::nullopt;
    }
    const float sign = (label[1] == '+') ? 1.0F : -1.0F;
    try {
        std::size_t consumed = 0;
        const float az = std::stof(label.substr(2), &consumed) * sign;
        if (2U + consumed != label.size()) {
            return std::nullopt; // trailing garbage (e.g. "M+030foo")
        }
        return std::make_pair(az, el);
    } catch (...) {
        return std::nullopt;
    }
}

struct HoaBlock {
    Hoa3Coeffs gains{};
    DiffuseSlots diffuse_gains{};
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool is_lfe{false};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

struct HoaFrameGains {
    Hoa3Coeffs direct{};
    DiffuseSlots diffuse{};
};

struct DiffuseState {
    // Stored [delay_pos][sh] (transposed vs the natural [sh][delay_pos]): a single decorrelation
    // tap then reads 16 contiguous SH samples, so the per-coefficient accumulation vectorises and
    // the FMA units pipeline across coefficients instead of serialising one 32-deep reduction per
    // coefficient. See add_diffuse_hoa().
    std::array<Hoa3Coeffs, k_diffuse_delay_len> delay_lines{};
    std::size_t write_pos{0};
};

enum class BlockFilter : uint8_t { all, lfe_only };

[[nodiscard]] bool block_matches_filter(const HoaBlock& block, BlockFilter filter) noexcept {
    return filter == BlockFilter::all || block.is_lfe;
}

struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::string object_id;     // owning SceneObject::id, for live gain overrides
    bool has_lfe_block{false}; // true when any ds_block is LFE; used for separate TP tracking
    bool has_diffuse_block{false};
    std::vector<HoaBlock> blocks; // sorted ascending by start_sample
};

// Returns linearly interpolated HOA gains at abs_frame for the given channel.
// Returns all-zero coefficients when abs_frame is outside every block.
[[nodiscard]] HoaFrameGains gains_at(const ChannelGainInfo& cg,
                                     uint64_t abs_frame,
                                     uint64_t default_interp,
                                     BlockFilter filter = BlockFilter::all) {
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
    if (!block_matches_filter(cur, filter)) {
        return {};
    }
    HoaFrameGains frame{cur.gains, cur.diffuse_gains};
    // Interpolation ramp: blend from previous block gains when jump_position is false.
    if (!cur.jump_position && cur_it != cg.blocks.begin()) {
        const HoaBlock& prev = *std::prev(cur_it);
        if (!block_matches_filter(prev, filter)) {
            return frame;
        }
        // Clamp interp_len to active block duration (mirrors EAR/VBAP interpolation_length()).
        uint64_t active_end = cur.end_sample;
        const auto next_it = std::next(cur_it);
        if (next_it != cg.blocks.end()) {
            active_end = std::min(active_end, next_it->start_sample);
        }
        const uint64_t active_len = (active_end > cur.start_sample) ? (active_end - cur.start_sample) : 0;
        const uint64_t interp_len = std::min(cur.interp_length_samples.value_or(default_interp), active_len);
        const uint64_t delta = abs_frame - cur.start_sample;
        if (interp_len > 0 && delta < interp_len) {
            const double alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
            HoaFrameGains result;
            for (std::size_t i = 0; i < k_hoa3_channels; ++i) {
                result.direct.at(i) = static_cast<float>((static_cast<double>(prev.gains.at(i)) * (1.0 - alpha)) +
                                                         (static_cast<double>(cur.gains.at(i)) * alpha));
            }
            for (std::size_t slot = 0; slot < k_diffuse_slots; ++slot) {
                for (std::size_t i = 0; i < k_hoa3_channels; ++i) {
                    result.diffuse.at(slot).at(i) =
                        static_cast<float>((static_cast<double>(prev.diffuse_gains.at(slot).at(i)) * (1.0 - alpha)) +
                                           (static_cast<double>(cur.diffuse_gains.at(slot).at(i)) * alpha));
                }
            }
            return result;
        }
    }
    return frame;
}

void add_direct_hoa(float in_s, const Hoa3Coeffs& gains, float* out_frame) {
    for (std::size_t out_ch = 0; out_ch < k_hoa3_channels; ++out_ch) {
        out_frame[out_ch] += in_s * gains.at(out_ch);
    }
}

void add_diffuse_hoa(const Hoa3Coeffs& diffuse_in, DiffuseState& state, float* MRADM_RESTRICT out_frame) {
    // Per-HOA-coefficient multi-tap decorrelation keeps objectDivergence direction
    // information separate instead of collapsing all diffuse energy into one mono bus.
    constexpr std::array<std::size_t, k_diffuse_dirs> k_delays = {
        37U,  53U,  67U,  83U,  97U,  109U, 127U, 149U, 163U, 181U, 199U, 211U, 233U, 251U, 271U, 293U,
        313U, 337U, 359U, 383U, 409U, 431U, 457U, 487U, 521U, 557U, 593U, 631U, 673U, 719U, 761U, 809U,
    };
    constexpr std::array<float, k_diffuse_dirs> k_polarity = {
        1.F,  -1.F, 1.F, 1.F,  -1.F, -1.F, 1.F,  -1.F, -1.F, 1.F,  1.F, -1.F, 1.F,  -1.F, -1.F, 1.F,
        -1.F, 1.F,  1.F, -1.F, 1.F,  -1.F, -1.F, 1.F,  1.F,  -1.F, 1.F, -1.F, -1.F, 1.F,  -1.F, 1.F,
    };
    // 1/sqrt(N) is constant across all calls; compute once instead of per frame/slot/channel.
    static const float k_cloud_weight = 1.0F / std::sqrt(static_cast<float>(k_diffuse_dirs));

    // Tap-outer / coefficient-inner: each tap reads one contiguous 16-float row of the (transposed)
    // delay line, and the inner loop accumulates 16 independent coefficient lanes. This lets the
    // compiler vectorise across coefficients and pipeline the FMA units, instead of evaluating one
    // 32-deep serial reduction per coefficient. Per coefficient the taps are still summed in order
    // 0..31 with the identical (sample * polarity) * cloud_weight factoring, so the output is
    // bit-identical to the previous coefficient-outer form.
    Hoa3Coeffs acc{};
    float* MRADM_RESTRICT acc_p = acc.data();
    for (std::size_t tap = 0; tap < k_diffuse_dirs; ++tap) {
        const std::size_t read_pos = (state.write_pos + k_diffuse_delay_len - k_delays.at(tap)) % k_diffuse_delay_len;
        const float polarity = k_polarity.at(tap);
        const float* MRADM_RESTRICT row = state.delay_lines.at(read_pos).data();
        for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
            acc_p[sh] += row[sh] * polarity * k_cloud_weight;
        }
    }
    for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
        out_frame[sh] += acc_p[sh];
    }
    state.delay_lines.at(state.write_pos) = diffuse_in;

    state.write_pos = (state.write_pos + 1U) % k_diffuse_delay_len;
}

// NOLINTNEXTLINE(readability-function-size)
std::vector<ChannelGainInfo> build_gain_matrix(const AdmScene& scene, LogSink& logs) {
    std::map<uint16_t, ChannelGainInfo> by_channel;

    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const uint16_t in_ch = track.channel_index.value();
            auto& cg = by_channel[in_ch];
            cg.input_channel = in_ch;
            cg.object_id = obj.id;

            for (const auto& raw_block : track.blocks) {
                const auto& off = obj.position_offset;
                SceneObjectBlock base = raw_block;
                if (off) {
                    base.position = apply_position_offset(base.position, *off);
                }

                Hoa3Coeffs sh{};
                DiffuseSlots diffuse_gains{};
                const auto sources = expand_object_divergence(base);
                for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
                    const auto& source = sources.at(source_index);
                    const Hoa3Coeffs source_sh = encode_extent(source.position, source);
                    const float combined_gain = source.gain * obj.gain;
                    const float diffuse = std::clamp(source.diffuse, 0.0F, 1.0F);
                    const float direct_gain = combined_gain * std::sqrt(1.0F - diffuse);
                    const float source_diffuse_gain = combined_gain * std::sqrt(diffuse);
                    const std::size_t diffuse_slot = sources.size() == k_diffuse_slots ? source_index : 1U;
                    SceneObjectBlock diffuse_source = source;
                    diffuse_source.width = std::max(diffuse_source.width, 1.0F);
                    diffuse_source.height = std::max(diffuse_source.height, 1.0F);
                    const Hoa3Coeffs diffuse_sh = encode_extent(diffuse_source.position, diffuse_source);
                    for (std::size_t i = 0; i < k_hoa3_channels; ++i) {
                        sh.at(i) += source_sh.at(i) * direct_gain;
                        diffuse_gains.at(diffuse_slot).at(i) += diffuse_sh.at(i) * source_diffuse_gain;
                    }
                }
                cg.has_diffuse_block =
                    cg.has_diffuse_block || std::ranges::any_of(diffuse_gains, [](const auto& coeffs) {
                        return std::fabs(coeffs.at(0)) > 0.0F;
                    });
                cg.blocks.push_back({sh,
                                     diffuse_gains,
                                     raw_block.start_sample,
                                     std::min(raw_block.end_sample, obj.end_sample),
                                     false,
                                     raw_block.jump_position,
                                     raw_block.interp_length_samples});
            }

            for (const auto& ds : track.ds_blocks) {
                Hoa3Coeffs sh{};
                bool is_lfe = false;
                if (render_common::direct_speakers_block_is_lfe(ds)) {
                    // LFE: no directionality → encode as omnidirectional (W channel only, ACN 0).
                    // Label check runs before position so channels like RC_LFE (no lowPass element
                    // but carrying a nominal position) are still treated as non-directional.
                    sh[0] = 1.0F;
                    is_lfe = true;
                    cg.has_lfe_block = true;
                } else if (ds.has_position) {
                    sh = encode_polar(ds.azimuth, ds.elevation);
                } else {
                    bool found_label_pos = false;
                    float parsed_azimuth = 0.F;
                    float parsed_elevation = 0.F;
                    for (const auto& label : ds.speaker_labels) {
                        const auto parsed_pos = parse_speaker_label(label);
                        if (parsed_pos.has_value()) {
                            parsed_azimuth = parsed_pos->first;
                            parsed_elevation = parsed_pos->second;
                            found_label_pos = true;
                            break;
                        }
                    }
                    if (!found_label_pos) {
                        logs.log(LogLevel::warning,
                                 "hoa-encode",
                                 fmt::format("DirectSpeakers channel {} has no position and no parseable label; "
                                             "skipping",
                                             in_ch));
                        continue;
                    }
                    sh = encode_polar(parsed_azimuth, parsed_elevation);
                }
                const float combined_gain = ds.gain * obj.gain;
                std::ranges::transform(sh, sh.begin(), [combined_gain](float c) { return c * combined_gain; });
                // DirectSpeakers positions are static — no interpolation needed between blocks.
                cg.blocks.push_back(
                    {sh, {}, ds.start_sample, std::min(ds.end_sample, obj.end_sample), is_lfe, true, std::nullopt});
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

// Encode one block [frames_done, frames_done+frames_now) of every channel's HOA
// contribution into out_block (k_hoa3_channels interleaved per frame; caller zeroes it).
// Carries the diffuse decorrelation delay lines (diffuse_states, per channel) across calls.
// Extracted verbatim from render_window's encode loop so the offline batch path and the
// realtime HoaStream share one implementation and cannot drift (bit-exactness contract).
void encode_hoa_block(const std::vector<ChannelGainInfo>& gain_matrix,
                      std::vector<std::array<DiffuseState, k_diffuse_slots>>& diffuse_states,
                      const float* in_block,
                      float* out_block,
                      uint64_t frames_done,
                      uint64_t frames_now,
                      uint16_t num_in_ch,
                      uint64_t default_interp,
                      uint32_t object_smoothing_frames) {
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& cg = gain_matrix.at(ci);
        auto& diffuse_state = diffuse_states.at(ci);
        const bool has_diffuse = cg.has_diffuse_block;
        if (object_smoothing_frames > 0) {
            const HoaFrameGains start_gains = gains_at(cg, frames_done, default_interp);
            const HoaFrameGains end_gains = gains_at(cg, frames_done + frames_now - 1, default_interp);
            for (std::size_t f = 0; f < frames_now; ++f) {
                const float alpha = frames_now > 1 ? static_cast<float>(f) / static_cast<float>(frames_now - 1) : 0.0F;
                const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
                float* out_frame = out_block + (f * k_hoa3_channels);
                Hoa3Coeffs direct_gains{};
                for (std::size_t out_ch = 0; out_ch < k_hoa3_channels; ++out_ch) {
                    direct_gains.at(out_ch) =
                        (start_gains.direct.at(out_ch) * (1.0F - alpha)) + (end_gains.direct.at(out_ch) * alpha);
                }
                add_direct_hoa(in_s, direct_gains, out_frame);
                if (has_diffuse) {
                    for (std::size_t slot = 0; slot < k_diffuse_slots; ++slot) {
                        Hoa3Coeffs diffuse_gains{};
                        for (std::size_t out_ch = 0; out_ch < k_hoa3_channels; ++out_ch) {
                            diffuse_gains.at(out_ch) = (start_gains.diffuse.at(slot).at(out_ch) * (1.0F - alpha)) +
                                                       (end_gains.diffuse.at(slot).at(out_ch) * alpha);
                            diffuse_gains.at(out_ch) *= in_s;
                        }
                        add_diffuse_hoa(diffuse_gains, diffuse_state.at(slot), out_frame);
                    }
                }
            }
            continue;
        }
        for (std::size_t f = 0; f < frames_now; ++f) {
            const uint64_t abs_frame = frames_done + f;
            const HoaFrameGains gains = gains_at(cg, abs_frame, default_interp);
            const float in_s = in_block[(f * num_in_ch) + cg.input_channel];
            float* out_frame = out_block + (f * k_hoa3_channels);
            add_direct_hoa(in_s, gains.direct, out_frame);
            if (has_diffuse) {
                for (std::size_t slot = 0; slot < k_diffuse_slots; ++slot) {
                    Hoa3Coeffs diffuse_gains = gains.diffuse.at(slot);
                    std::ranges::transform(
                        diffuse_gains, diffuse_gains.begin(), [in_s](float coeff) { return coeff * in_s; });
                    add_diffuse_hoa(diffuse_gains, diffuse_state.at(slot), out_frame);
                }
            }
        }
    }
}

// Immutable, reusable HOA state: the per-object HOA encode gain matrix (the expensive
// part). Reused across render_window() calls (PreviewSession scrubbing). The AllRAD
// meter-decode matrix and the diffuse delay lines are cheap / per-output, so they stay
// in render_window.
struct HoaPrepared final : IPreparedRender {
    std::vector<ChannelGainInfo> gain_matrix;
};

// Realtime streaming HOA session over the same prepared encode gain matrix as
// render_window. It encodes k_block_size-aligned blocks via the SAME encode_hoa_block the
// offline path uses (carrying the diffuse decorrelation delay lines across blocks) into a
// FIFO of k_hoa3_channels SH channels — the encoded ambisonic signal, identical to what
// render_window writes (the AllRAD 7.1.4 decode is metering-only and not part of the
// output). A gap-free run from frame 0 is bit-identical to render_window. seek() resets the
// diffuse delay lines (a small discontinuity, acceptable for monitoring), seeding the
// circular write position to frame % delay-len to match the offline windowed indexing.
// set_overrides applies live per-object gain by pre-scaling the matching input channels
// before the (linear) HOA encode — equal to scaling the object gain, and it scales the
// object's direct + diffuse alike. Topology scales are not yet wired for HOA.
class HoaStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<HoaStream>>
    create(const HoaPrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        auto reader = bw64::readFile(plan.input_path);
        if (!reader) {
            return make_error(
                ErrorCode::io_error, "failed to open input for realtime hoa stream", "input=" + plan.input_path);
        }
        (void) logs;
        return std::unique_ptr<HoaStream>{new HoaStream(prepared, std::move(reader), plan)};
    }

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (fifo_read_ >= fifo_.size()) {
                if (frames_done_ >= total_frames_) {
                    break;
                }
                render_block();
                if (fifo_read_ >= fifo_.size()) {
                    break;
                }
            }
            const std::size_t avail = (fifo_.size() - fifo_read_) / k_hoa3_channels;
            const std::size_t take = std::min(frames - produced, avail);
            std::copy_n(fifo_.data() + fifo_read_, take * k_hoa3_channels, out.data() + (produced * k_hoa3_channels));
            fifo_read_ += take * k_hoa3_channels;
            produced += take;
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        frames_done_ = std::min(frame, total_frames_);
        const auto write_pos = static_cast<std::size_t>(frames_done_ % k_diffuse_delay_len);
        for (auto& slots : diffuse_states_) {
            for (auto& st : slots) {
                st = DiffuseState{};
                st.write_pos = write_pos;
            }
        }
        render_common::seek_reader_abs(*reader_, frames_done_);
        fifo_.clear();
        fifo_read_ = 0;
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        std::unordered_map<std::string, float> live;
        for (const auto& ov : overrides.objects) {
            live[ov.object_id] = std::pow(10.0F, ov.gain_db / 20.0F);
        }
        std::ranges::fill(channel_gain_, 1.0F);
        any_live_gain_ = false;
        // Semantic boundary: the override is projected object → input channel (each channel
        // scaled by its owning object's gain). ADM's gain matrix is one object per input
        // channel, so this is exact today; if independently-overridable objects ever shared
        // one input channel they would scale together (last writer wins) — revisit then.
        for (const auto& cg : prepared_.gain_matrix) {
            if (const auto it = live.find(cg.object_id); it != live.end() && cg.input_channel < channel_gain_.size()) {
                channel_gain_[cg.input_channel] = it->second;
                any_live_gain_ = true;
            }
        }
    }

    [[nodiscard]] uint32_t out_channels() const override { return k_hoa3_channels_u16; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return "hoa3"; }

  private:
    HoaStream(const HoaPrepared& prepared, std::unique_ptr<bw64::Bw64Reader> reader, const RenderPlan& plan)
        : prepared_(prepared), reader_(std::move(reader)), num_in_ch_(plan.scene.info.num_channels),
          sample_rate_(plan.scene.info.sample_rate), total_frames_(plan.scene.info.num_frames),
          object_smoothing_frames_(plan.object_smoothing_frames),
          k_block_size_(std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          default_interp_(static_cast<uint64_t>(plan.scene.info.sample_rate) * plan.default_interp_ms / 1000U),
          diffuse_states_(prepared.gain_matrix.size()),
          in_block_(static_cast<std::size_t>(plan.scene.info.num_channels) *
                    std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          channel_gain_(plan.scene.info.num_channels, 1.0F) {}

    void apply_live_gain(uint64_t frames_now) {
        if (!any_live_gain_) {
            return;
        }
        for (std::size_t f = 0; f < frames_now; ++f) {
            float* frame = in_block_.data() + (f * num_in_ch_);
            for (uint16_t c = 0; c < num_in_ch_; ++c) {
                frame[c] *= channel_gain_[c];
            }
        }
    }

    void render_block() {
        const uint64_t frames_now = std::min<uint64_t>(k_block_size_, total_frames_ - frames_done_);
        reader_->read(in_block_.data(), frames_now);
        apply_live_gain(frames_now);
        fifo_.assign(k_hoa3_channels * static_cast<std::size_t>(frames_now), 0.0F);
        fifo_read_ = 0;
        encode_hoa_block(prepared_.gain_matrix,
                         diffuse_states_,
                         in_block_.data(),
                         fifo_.data(),
                         frames_done_,
                         frames_now,
                         num_in_ch_,
                         default_interp_,
                         static_cast<uint32_t>(object_smoothing_frames_));
        frames_done_ += frames_now;
    }

    const HoaPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<bw64::Bw64Reader> reader_;
    uint16_t num_in_ch_;
    uint32_t sample_rate_;
    uint64_t total_frames_;
    uint64_t object_smoothing_frames_;
    uint64_t k_block_size_;
    uint64_t default_interp_;
    std::vector<std::array<DiffuseState, k_diffuse_slots>> diffuse_states_;
    std::vector<float> in_block_;
    std::vector<float> channel_gain_; // per-input-channel live gain multiplier (1.0 = none)
    bool any_live_gain_{false};
    std::vector<float> fifo_;
    std::size_t fifo_read_{0};
    uint64_t frames_done_{0};
};

class HoaRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const HoaPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "hoa-encode: open_stream received an incompatible prepared state", {});
        }
        auto stream = HoaStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

CapabilityReport HoaRenderer::capabilities() const {
    return hoa_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> HoaRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    if (plan.output_layout != "hoa3") {
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported HOA output layout '{}'; supported: hoa3", plan.output_layout),
                          {});
    }

    auto gain_matrix = build_gain_matrix(plan.scene, logs);
    if (gain_matrix.empty()) {
        logs.log(LogLevel::warning, "hoa-encode", "no renderable tracks found (all muted?), writing silence");
    }

    const auto num_in_ch = plan.scene.info.num_channels;
    const auto invalid_channel =
        std::ranges::find_if(gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
    if (invalid_channel != gain_matrix.end()) {
        return make_error(ErrorCode::render_failed,
                          fmt::format("track channel index {} is outside input channel count {}",
                                      invalid_channel->input_channel,
                                      num_in_ch),
                          "input=" + plan.input_path);
    }

    auto prepared = std::make_shared<HoaPrepared>();
    prepared->gain_matrix = std::move(gain_matrix);
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> HoaRenderer::render_window(const IPreparedRender& prep,
                                                 const RenderPlan& plan,
                                                 ProgressSink& progress,
                                                 LogSink& logs) { // NOLINT(readability-function-size)
    const auto* prepared = dynamic_cast<const HoaPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(
            ErrorCode::internal_error, "hoa-encode: render_window received an incompatible prepared state", {});
    }
    const auto& gain_matrix = prepared->gain_matrix;

    const auto& info = plan.scene.info;
    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;
    constexpr uint16_t k_num_out = k_hoa3_channels_u16;

    // Build 7.1.4 AllRAD decode matrix for BS.1770 playback-domain measurement.
    // LFE is NOT a spatial speaker — it is excluded from the AllRAD matrix and its
    // decoded slot is zeroed.  ebur128 is initialised for all 12 channels with explicit
    // channel-type assignments so that BS.1770 weighting is applied correctly.
    //
    // SAF getLoudspeakerDecoderMtx uses N3D internally; our HOA output is SN3D.
    // Each column of the decode matrix is scaled by sqrt(2n+1) to compensate.
    constexpr int k_714_nls = 11; // non-LFE spatial speakers for AllRAD
    constexpr int k_714_ch = 12;  // ebur128 channel count (11 spatial + LFE slot)
    constexpr auto k_714_nls_sz = static_cast<std::size_t>(k_714_nls);
    constexpr auto k_714_ch_sz = static_cast<std::size_t>(k_714_ch);
    // clang-format off
    // 11 non-LFE speakers; LFE (az=45, el=-30) deliberately omitted.
    constexpr std::array<float, k_714_nls_sz * 2> k_714_dirs = {
         30.F,   0.F,   // row 0 → ch0  L
        -30.F,   0.F,   // row 1 → ch1  R
          0.F,   0.F,   // row 2 → ch2  C
         90.F,   0.F,   // row 3 → ch4  Ls   (ch3 = LFE slot, zeroed)
        -90.F,   0.F,   // row 4 → ch5  Rs
        135.F,   0.F,   // row 5 → ch6  Lss
       -135.F,   0.F,   // row 6 → ch7  Rss
         45.F,  30.F,   // row 7 → ch8  Ltf
        -45.F,  30.F,   // row 8 → ch9  Rtf
        135.F,  30.F,   // row 9 → ch10 Ltr
       -135.F,  30.F,   // row10 → ch11 Rtr
    };
    constexpr float k_sqrt5 = 2.2360679774997896F;
    constexpr float k_sqrt7 = 2.6457513110645905F;
    constexpr std::array<float, k_hoa3_channels> k_sn3d_to_n3d = {
        1.F,                                                                                     // n=0 (√1)
        std::numbers::sqrt3_v<float>, std::numbers::sqrt3_v<float>, std::numbers::sqrt3_v<float>, // n=1 (√3)
        k_sqrt5, k_sqrt5, k_sqrt5, k_sqrt5, k_sqrt5,                                            // n=2 (√5)
        k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7, k_sqrt7,                          // n=3 (√7)
    };
    // clang-format on
    std::array<float, k_714_nls_sz * k_hoa3_channels> dec_mtx{};
    {
        // getLoudspeakerDecoderMtx takes a non-const pointer; copy to a local mutable array.
        std::array<float, k_714_nls_sz * 2> dirs_buf = k_714_dirs;
        getLoudspeakerDecoderMtx(dirs_buf.data(), k_714_nls, LOUDSPEAKER_DECODER_ALLRAD, 3, 1, dec_mtx.data());
        for (int ls = 0; ls < k_714_nls; ++ls) {
            for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                dec_mtx.at((static_cast<std::size_t>(ls) * k_hoa3_channels) + sh) *= k_sn3d_to_n3d.at(sh);
            }
        }
    }

    try {
        logs.log(LogLevel::info,
                 "hoa-encode",
                 fmt::format("encoding {} input channels (Objects + DirectSpeakers) → HOA3 ({} ch), {} frames",
                             gain_matrix.size(),
                             k_num_out,
                             num_frames));
        progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.3, 0.0, 0, 0, "encoding HOA"});

        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, k_num_out, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);
        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;
        std::vector<float> measure_hoa_block(static_cast<std::size_t>(k_num_out) * k_block_size);
        std::vector<float> decoded_block(k_714_ch_sz * k_block_size);
        std::vector<std::array<DiffuseState, k_diffuse_slots>> diffuse_states(gain_matrix.size());

        struct EburFree {
            void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
        };
        using EburPtr = std::unique_ptr<ebur128_state, EburFree>;
        EburPtr lufs_st{ebur128_init(static_cast<unsigned int>(k_714_ch),
                                     static_cast<unsigned long>(sample_rate),
                                     EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};
        if (lufs_st) {
            // Explicit 7.1.4 channel map so BS.1770 weighting is applied correctly.
            // Channels 6+ default to UNUSED in libebur128; set all 12 explicitly.
            ebur128_set_channel(lufs_st.get(), 0U, EBUR128_Mp030);  // L
            ebur128_set_channel(lufs_st.get(), 1U, EBUR128_Mm030);  // R
            ebur128_set_channel(lufs_st.get(), 2U, EBUR128_Mp000);  // C
            ebur128_set_channel(lufs_st.get(), 3U, EBUR128_UNUSED); // LFE (zeroed, excluded)
            ebur128_set_channel(lufs_st.get(), 4U, EBUR128_Mp090);  // Ls
            ebur128_set_channel(lufs_st.get(), 5U, EBUR128_Mm090);  // Rs
            ebur128_set_channel(lufs_st.get(), 6U, EBUR128_Mp135);  // Lss
            ebur128_set_channel(lufs_st.get(), 7U, EBUR128_Mm135);  // Rss
            ebur128_set_channel(lufs_st.get(), 8U, EBUR128_Up045);  // Ltf
            ebur128_set_channel(lufs_st.get(), 9U, EBUR128_Um045);  // Rtf
            ebur128_set_channel(lufs_st.get(), 10U, EBUR128_Up135); // Ltr
            ebur128_set_channel(lufs_st.get(), 11U, EBUR128_Um135); // Rtr
        }

        // Separate TP tracker for LFE channels. LFE is still encoded W-only in the HOA
        // output, but it is subtracted from the HOA measurement buffer before the 7.1.4
        // decode so it cannot contribute to LUFS/spatial TP. Its peak is measured on this
        // mono TP-only state and merged with the spatial decode peak at the end.
        EburPtr lfe_tp_st;
        std::vector<float> lfe_mix_block;
        const bool has_lfe = std::ranges::any_of(gain_matrix, [](const auto& cg) { return cg.has_lfe_block; });
        if (has_lfe) {
            lfe_tp_st.reset(ebur128_init(1U, static_cast<unsigned long>(sample_rate), EBUR128_MODE_TRUE_PEAK));
            lfe_mix_block.resize(k_block_size, 0.F);
        }

        // Loudness / true-peak measurement (LFE TP + 7.1.4 decode + two ebur128 states) is run on a
        // background thread so it overlaps the next block's HOA encode. The measurement only reads the
        // input and output buffers (both double-buffered below) plus const scene data; its scratch and
        // ebur128 states are touched solely by the worker. Running blocks in FIFO order keeps the
        // measured loudness / true peak bit-identical to the inline version.
        const auto measure_block = [&](const float* in_data, const float* out_data, uint64_t fd, uint64_t fn) {
            const std::size_t measure_samples = static_cast<std::size_t>(k_num_out) * fn;
            if (lufs_st) {
                std::copy_n(out_data, measure_samples, measure_hoa_block.begin());
            }
            if (lfe_tp_st) {
                std::fill(lfe_mix_block.begin(), lfe_mix_block.begin() + static_cast<std::ptrdiff_t>(fn), 0.F);
                for (const auto& cg : gain_matrix) {
                    if (!cg.has_lfe_block) {
                        continue;
                    }
                    for (std::size_t f = 0; f < fn; ++f) {
                        const float in_s = in_data[(f * num_in_ch) + cg.input_channel];
                        const HoaFrameGains lfe_gains = gains_at(cg, fd + f, k_default_interp, BlockFilter::lfe_only);
                        lfe_mix_block[f] += in_s * lfe_gains.direct[0];
                        if (lufs_st) {
                            float* measure_hoa = measure_hoa_block.data() + (f * k_hoa3_channels);
                            for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                                measure_hoa[sh] -= in_s * lfe_gains.direct.at(sh);
                            }
                        }
                    }
                }
                ebur128_add_frames_float(lfe_tp_st.get(), lfe_mix_block.data(), static_cast<std::size_t>(fn));
            }
            if (lufs_st) {
                // Decode 16ch HOA → 12ch 7.1.4 for BS.1770 playback-domain measurement.
                // rows 0-2  → ch 0-2 (L R C); ch3 (LFE) = 0; rows 3-10 → ch 4-11.
                for (std::size_t f = 0; f < fn; ++f) {
                    const float* hoa = measure_hoa_block.data() + (f * k_hoa3_channels);
                    float* dec = decoded_block.data() + (f * k_714_ch_sz);
                    for (int ls = 0; ls < 3; ++ls) {
                        float s = 0.F;
                        const float* row = dec_mtx.data() + (static_cast<std::size_t>(ls) * k_hoa3_channels);
                        for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                            s += row[sh] * hoa[sh];
                        }
                        dec[ls] = s;
                    }
                    dec[3] = 0.F; // LFE not decoded
                    for (int ls = 3; ls < k_714_nls; ++ls) {
                        float s = 0.F;
                        const float* row = dec_mtx.data() + (static_cast<std::size_t>(ls) * k_hoa3_channels);
                        for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                            s += row[sh] * hoa[sh];
                        }
                        dec[ls + 1] = s; // +1 to skip LFE slot at ch3
                    }
                }
                ebur128_add_frames_float(lufs_st.get(), decoded_block.data(), static_cast<std::size_t>(fn));
            }
        };

        // Double-buffer the input and output blocks so the next block can be encoded while the meter
        // still reads the previous block's buffers; reuse waits on the outstanding measurement future.
        constexpr std::size_t k_num_buffers = 2;
        std::array<std::vector<float>, k_num_buffers> in_buffers;
        std::array<std::vector<float>, k_num_buffers> out_buffers;
        for (auto& buffer : in_buffers) {
            buffer.assign(static_cast<std::size_t>(num_in_ch) * k_block_size, 0.0F);
        }
        for (auto& buffer : out_buffers) {
            buffer.assign(static_cast<std::size_t>(k_num_out) * k_block_size, 0.0F);
        }
        std::array<std::future<void>, k_num_buffers> meter_pending;
        render_common::SerialWorker meter;
        std::size_t buf_idx = 0;

        // On-demand output window (RenderPlan::render_window). HOA's diffuse path has a
        // k_diffuse_delay_len-tap delay line and its smoothing samples gains at block
        // edges, so blocks are processed on the same k_block_size grid as a full render
        // and one aligned block (>= the 1024-tap delay) is pre-rolled before the window.
        // The delay line is circular, indexed by absolute frame mod k_diffuse_delay_len,
        // so each state's write_pos is seeded to start_pos % k_diffuse_delay_len to match
        // the full render exactly; the pre-roll block then refills the line. Direct (non-
        // diffuse) gains are closed-form per absolute frame. When not windowed,
        // win_start=0 / win_end=num_frames reproduces the full-timeline encode.
        const bool windowed = plan.render_window.has_value();
        const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
        const uint64_t win_end =
            windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
        uint64_t start_pos = 0;
        if (windowed && win_start >= k_block_size) {
            start_pos = ((win_start / k_block_size) - 1) * k_block_size; // one aligned pre-roll block
        }
        if (start_pos > 0) {
            render_common::seek_reader_abs(*reader, start_pos);
            const auto init_write_pos = static_cast<std::size_t>(start_pos % k_diffuse_delay_len);
            for (auto& slots : diffuse_states) {
                for (auto& st : slots) {
                    st.write_pos = init_write_pos;
                }
            }
        }
        const uint64_t progress_total = std::max<uint64_t>(1, win_end - start_pos);
        const auto progress_span = static_cast<double>(progress_total);
        uint64_t frames_done = start_pos;

        while (frames_done < win_end) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(k_num_out) * frames_now;

            // Sub-range of this block inside the output window [win_start, win_end).
            const uint64_t w_lo = std::max(frames_done, win_start);
            const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
            const bool emit = w_hi > w_lo;
            const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
            const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;

            // Reclaim this block's buffers once the meter has finished its previous use of them.
            if (meter_pending.at(buf_idx).valid()) {
                meter_pending.at(buf_idx).get();
            }
            std::vector<float>& in_block = in_buffers.at(buf_idx);
            std::vector<float>& out_block = out_buffers.at(buf_idx);

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            encode_hoa_block(gain_matrix,
                             diffuse_states,
                             in_block.data(),
                             out_block.data(),
                             frames_done,
                             frames_now,
                             num_in_ch,
                             k_default_interp,
                             plan.object_smoothing_frames);

            // Write only the in-window frames; pre-roll blocks (emit == false) warm the
            // diffuse delay line but are not written.
            if (emit && writer.write(out_block.data() + (emit_off * k_num_out), emit_count) != emit_count) {
                return make_error(ErrorCode::io_error, "short write while encoding HOA", "output=" + plan.output_path);
            }

            // Offload loudness / true-peak measurement to the background meter (overlaps next block).
            // Windowed: measure exactly the written frames. Otherwise honor meter_window. Either way
            // pass the absolute start frame so the LFE gain lookups stay correct.
            if (lufs_st || lfe_tp_st) {
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
                    const float* in_data = in_block.data() + (meter_off * num_in_ch);
                    const float* out_data = out_block.data() + (meter_off * k_num_out);
                    const uint64_t fd = frames_done + meter_off;
                    const uint64_t fn = meter_count;
                    meter_pending.at(buf_idx) = meter.post(
                        [&measure_block, in_data, out_data, fd, fn] { measure_block(in_data, out_data, fd, fn); });
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
                                  "encoding"});
        }

        // All audio is written; wait for outstanding measurements before reading global metrics.
        for (auto& pending : meter_pending) {
            if (pending.valid()) {
                pending.get();
            }
        }

        progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
        logs.log(LogLevel::info,
                 "hoa-encode",
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
            for (unsigned int ch = 0; ch < static_cast<unsigned int>(k_714_ch); ++ch) {
                double ch_peak = 0.0;
                if (ebur128_true_peak(lufs_st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, ch_peak);
                }
            }
            if (lfe_tp_st) {
                double lfe_peak = 0.0;
                if (ebur128_true_peak(lfe_tp_st.get(), 0U, &lfe_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, lfe_peak);
                }
            }
            if (max_peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(max_peak);
            }
        }
        return metrics;

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
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    r.supports_render_window = true; // block-aligned seek + 1 pre-roll block (diffuse delay line)
    r.supported_layouts = {
        {"hoa3", "HOA 3rd Order (16ch, ACN/SN3D)", 16, true, 0, false},
    };
    return r;
}

std::unique_ptr<IRenderer> create_hoa_renderer() {
    return std::make_unique<HoaRenderer>();
}

} // namespace mradm
