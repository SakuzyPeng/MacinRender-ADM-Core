#pragma once
// Frozen ba96bac numerical reference; tests only.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "meter.h"
// clang-format off
#include "dsp.h"
// clang-format on

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_hoa.h"

#include "consistency_trace.h"
#include "render_common.h"


namespace mradm::hoa_legacy {
[[nodiscard]] inline float canonical_vector_length(float x, float y, float z) noexcept {
    const double dx = x;
    const double dy = y;
    const double dz = z;
    return static_cast<float>(std::sqrt(((dx * dx) + (dy * dy)) + (dz * dz)));
}

struct ExtentDiskSample {
    float x{0.0F};
    float y{0.0F};
    float weight{0.0F}; // linear gain weight (active weights sum to 1)
};

inline constexpr float k_extent_disk_outer_weight = 1.0F / 12.0F; // outer ring total = 2/3
inline constexpr float k_extent_disk_inner_weight = 1.0F / 24.0F; // inner ring total = 1/3

inline constexpr std::array<ExtentDiskSample, 17> k_extent_disk_samples{{
    {0.0F, 0.0F, 0.0F},
    {1.0F, 0.0F, k_extent_disk_outer_weight},
    {-1.0F, 0.0F, k_extent_disk_outer_weight},
    {0.0F, 1.0F, k_extent_disk_outer_weight},
    {0.0F, -1.0F, k_extent_disk_outer_weight},
    {0.70710678F, 0.70710678F, k_extent_disk_outer_weight},
    {-0.70710678F, 0.70710678F, k_extent_disk_outer_weight},
    {0.70710678F, -0.70710678F, k_extent_disk_outer_weight},
    {-0.70710678F, -0.70710678F, k_extent_disk_outer_weight},
    {0.5F, 0.0F, k_extent_disk_inner_weight},
    {-0.5F, 0.0F, k_extent_disk_inner_weight},
    {0.0F, 0.5F, k_extent_disk_inner_weight},
    {0.0F, -0.5F, k_extent_disk_inner_weight},
    {0.35355339F, 0.35355339F, k_extent_disk_inner_weight},
    {-0.35355339F, 0.35355339F, k_extent_disk_inner_weight},
    {0.35355339F, -0.35355339F, k_extent_disk_inner_weight},
    {-0.35355339F, -0.35355339F, k_extent_disk_inner_weight},
}};

struct ExtentRadii {
    float width_radius;
    float height_radius;
};
ExtentRadii extent_disk_radii(float width, float height, float depth, float distance) {
    // Distance-dependent spread scaling: nearer objects subtend a wider angle.
    const float spread_scale = std::clamp(1.0F / std::max(0.4F, distance), 0.5F, 2.5F);
    const float depth_radius = std::max(0.0F, depth) * 20.0F * spread_scale;
    const float width_radius = (std::max(0.0F, width) * 60.0F * spread_scale) + depth_radius;
    const float height_radius = (std::max(0.0F, height) * 45.0F * spread_scale) + depth_radius;
    return {width_radius, height_radius};
}

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
    const float len = std::max(1.0e-6F, canonical_vector_length(v.x, v.y, v.z));
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
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    consistency::dump("hoa.01-polar.f32", {az_deg, el_deg, az, el, cos_el, std::cos(az), std::sin(az), std::sin(el)});
#endif
    // Standard ADM: +az = left (CCW) → sin(az) gives positive Y for left sources.
    return {cos_el * std::cos(az), cos_el * std::sin(az), std::sin(el)};
}

// ADM Cartesian (X=right, Y=front, Z=up) → HOA (X=front, Y=left, Z=up).
Vec3 direction_from_cartesian(float xc, float yc, float zc) noexcept {
    return normalize({yc, -xc, zc});
}

Hoa3Coeffs encode_direction(Vec3 dir) noexcept {
    const Vec3 n = normalize(dir);
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    consistency::dump("hoa.02-direction.f32", {dir.x, dir.y, dir.z});
    consistency::dump("hoa.03-normalized.f32", {n.x, n.y, n.z});
    const auto coeffs = sh_sn3d_3(n.x, n.y, n.z);
    consistency::dump("hoa.04-coefficients.f32", std::span<const float>(coeffs));
#endif
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
    return pos.cartesian ? canonical_vector_length(pos.x, pos.y, pos.z) : pos.distance;
}

[[nodiscard]] Hoa3Coeffs encode_extent(const SceneBlockPosition& pos, const SceneObjectBlock& block) {
    const float distance = distance_from_position(pos);
    const auto [width_radius, height_radius] = extent_disk_radii(block.width, block.height, block.depth, distance);
    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        return encode_direction(direction_from_position(pos));
    }

    // Shared 17-point disk cloud (render_common); the per-backend geometry below
    // (direction_from_position / normalize / encode_direction) stays local and unchanged so
    // the HOA output remains bit-identical.
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const auto& k_samples = k_extent_disk_samples;

    const Vec3 center = direction_from_position(pos);
    Vec3 horizontal = cross({0.0F, 0.0F, 1.0F}, center);
    if (canonical_vector_length(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
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


inline void measure(const std::vector<ChannelGainInfo>& gain_matrix,
                    const float* in_data,
                    const float* out_data,
                    uint64_t fd,
                    uint64_t fn,
                    uint16_t num_in_ch,
                    uint64_t k_default_interp,
                    std::span<const float> dec_mtx,
                    std::vector<float>& measure_hoa_block,
                    std::vector<float>& decoded_block,
                    std::vector<float>& lfe_mix_block) {
    constexpr auto k_num_out = k_hoa3_channels;
    constexpr int k_714_nls = 11;
    constexpr std::size_t k_714_ch_sz = 12;
    const bool has_lfe = std::ranges::any_of(gain_matrix, [](const auto& cg) { return cg.has_lfe_block; });
    const std::size_t measure_samples = static_cast<std::size_t>(k_num_out) * fn;
    std::copy_n(out_data, measure_samples, measure_hoa_block.begin());
    if (has_lfe) {
        std::fill(lfe_mix_block.begin(), lfe_mix_block.begin() + static_cast<std::ptrdiff_t>(fn), 0.F);
        for (const auto& cg : gain_matrix) {
            if (!cg.has_lfe_block) {
                continue;
            }
            for (std::size_t f = 0; f < fn; ++f) {
                const float in_s = in_data[(f * num_in_ch) + cg.input_channel];
                const HoaFrameGains lfe_gains = gains_at(cg, fd + f, k_default_interp, BlockFilter::lfe_only);
                lfe_mix_block[f] += in_s * lfe_gains.direct[0];
                float* measure_hoa = measure_hoa_block.data() + (f * k_hoa3_channels);
                for (std::size_t sh = 0; sh < k_hoa3_channels; ++sh) {
                    measure_hoa[sh] -= in_s * lfe_gains.direct.at(sh);
                }
            }
        }
    }
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
}

} // namespace mradm::hoa_legacy
