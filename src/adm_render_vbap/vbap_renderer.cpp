#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <saf_vbap.h>
#include <string>
#include <string_view>
#include <vector>

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_vbap.h"

namespace mradm {

namespace {

// Internal layout alias so the rest of the renderer can use VbapSpeakerSpec directly.
using SpeakerDirection = VbapSpeakerSpec;

struct LayoutSpec {
    std::vector<VbapSpeakerSpec> speakers;
};

struct RegistryEntry {
    std::string id;
    std::string display_name;
    std::vector<VbapSpeakerSpec> speakers;
};

// Flat ordered registry of all VBAP layouts (built-ins first, then custom).
// Initialised once from built_default_registry(); mutable for register_vbap_layout().
std::vector<RegistryEntry>& layout_registry() {
    // clang-format off
    static auto reg = [] {
        std::vector<RegistryEntry> r;
        r.push_back({"0+2+0", "Stereo",
                     {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}}});
        r.push_back({"0+5+0", "5.1",
                     {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}, {0.0F, 0.0F, "M+000"},
                      {45.0F, -30.0F, "LFE1", true}, {110.0F, 0.0F, "M+110"}, {-110.0F, 0.0F, "M-110"}}});
        r.push_back({"0+7+0", "7.1",
                     {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}, {0.0F, 0.0F, "M+000"},
                      {45.0F, -30.0F, "LFE1", true}, {90.0F, 0.0F, "M+090"}, {-90.0F, 0.0F, "M-090"},
                      {135.0F, 0.0F, "M+135"}, {-135.0F, 0.0F, "M-135"}}});
        r.push_back({"4+5+0", "5.1.4",
                     {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}, {0.0F, 0.0F, "M+000"},
                      {45.0F, -30.0F, "LFE1", true}, {110.0F, 0.0F, "M+110"}, {-110.0F, 0.0F, "M-110"},
                      {30.0F, 30.0F, "U+030"}, {-30.0F, 30.0F, "U-030"}, {110.0F, 30.0F, "U+110"}, {-110.0F, 30.0F, "U-110"}}});
        r.push_back({"4+7+0", "7.1.4",
                     {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}, {0.0F, 0.0F, "M+000"},
                      {45.0F, -30.0F, "LFE1", true}, {90.0F, 0.0F, "M+090"}, {-90.0F, 0.0F, "M-090"},
                      {135.0F, 0.0F, "M+135"}, {-135.0F, 0.0F, "M-135"},
                      {45.0F, 30.0F, "U+045"}, {-45.0F, 30.0F, "U-045"}, {135.0F, 30.0F, "U+135"}, {-135.0F, 30.0F, "U-135"}}});
        r.push_back({"9.1.6", "9.1.6 (Dolby Atmos)",
                     {{30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}, {0.0F, 0.0F, "M+000"},
                      {45.0F, -30.0F, "LFE1", true},
                      {110.0F, 0.0F, "M+110"}, {-110.0F, 0.0F, "M-110"},
                      {150.0F, 0.0F, "M+150"}, {-150.0F, 0.0F, "M-150"},
                      {70.0F, 0.0F, "M+070"}, {-70.0F, 0.0F, "M-070"},
                      {70.0F, 45.0F, "U+070"}, {-70.0F, 45.0F, "U-070"},
                      {110.0F, 45.0F, "U+110"}, {-110.0F, 45.0F, "U-110"},
                      {150.0F, 45.0F, "U+150"}, {-150.0F, 45.0F, "U-150"}}});
        r.push_back({"9+10+3", "22.2",
                     {{60.0F, 0.0F, "M+060"}, {-60.0F, 0.0F, "M-060"}, {0.0F, 0.0F, "M+000"},
                      {45.0F, -30.0F, "LFE1", true}, {135.0F, 0.0F, "M+135"}, {-135.0F, 0.0F, "M-135"},
                      {30.0F, 0.0F, "M+030"}, {-30.0F, 0.0F, "M-030"}, {180.0F, 0.0F, "M+180"},
                      {-45.0F, -30.0F, "LFE2", true}, {90.0F, 0.0F, "M+090"}, {-90.0F, 0.0F, "M-090"},
                      {45.0F, 30.0F, "U+045"}, {-45.0F, 30.0F, "U-045"}, {0.0F, 30.0F, "U+000"},
                      {0.0F, 90.0F, "T+000"}, {135.0F, 30.0F, "U+135"}, {-135.0F, 30.0F, "U-135"},
                      {90.0F, 30.0F, "U+090"}, {-90.0F, 30.0F, "U-090"}, {180.0F, 30.0F, "U+180"},
                      {0.0F, -30.0F, "B+000"}, {45.0F, -30.0F, "B+045"}, {-45.0F, -30.0F, "B-045"}}});
        return r;
    }();
    // clang-format on
    return reg;
}

struct BlockGains {
    std::vector<float> gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

// One input channel with its full sorted block sequence.
struct ChannelGainInfo {
    uint16_t input_channel{0};
    std::vector<BlockGains> blocks; // sorted by start_sample
};

struct AccumulateContext {
    const float* input{nullptr};
    std::vector<float>* output{nullptr};
    uint64_t frames_done{0};
    uint16_t num_in_ch{0};
    uint16_t num_out_ch{0};
    uint64_t default_interp{0};
};

struct SafFree {
    // SAF allocates gain tables through its C API; ownership transfers to caller.
    void operator()(float* ptr) const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        std::free(ptr);
    }
};

[[nodiscard]] std::optional<LayoutSpec> layout_spec(std::string_view layout_id) {
    const auto& reg = layout_registry();
    const auto it = std::ranges::find_if(reg, [layout_id](const RegistryEntry& e) { return e.id == layout_id; });
    if (it == reg.end()) {
        return std::nullopt;
    }
    return LayoutSpec{it->speakers};
}

[[nodiscard]] bool is_2d_layout(const LayoutSpec& layout) {
    // LFE speakers sit at el=-30° but are not panned; exclude them from the check.
    return std::ranges::all_of(
        layout.speakers, [](const auto& speaker) { return speaker.is_lfe || std::fabs(speaker.elevation) < 1.0e-6F; });
}

[[nodiscard]] std::vector<float> flatten_layout(const LayoutSpec& layout) {
    // Only non-LFE speakers participate in VBAP panning.
    std::vector<float> result;
    result.reserve(layout.speakers.size() * 2U);
    for (const auto& speaker : layout.speakers) {
        if (!speaker.is_lfe) {
            result.push_back(speaker.azimuth);
            result.push_back(speaker.elevation);
        }
    }
    return result;
}

// Find the speaker index closest to (azimuth, elevation) by squared Euclidean
// distance in az/el space.  Azimuth is circular: -180 and +180 are adjacent.
// Good enough for speaker-routing fallback; not used for panning, so full
// great-circle accuracy is unnecessary.
[[nodiscard]] std::size_t nearest_speaker_index(const LayoutSpec& layout, float azimuth, float elevation) {
    std::size_t best = 0;
    float best_sq = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < layout.speakers.size(); ++i) {
        if (layout.speakers[i].is_lfe) {
            continue; // Non-LFE sources must never land on an LFE channel
        }
        const float daz = std::remainder(azimuth - layout.speakers[i].azimuth, 360.0F);
        const float del = elevation - layout.speakers[i].elevation;
        const float sq = (daz * daz) + (del * del);
        if (sq < best_sq) {
            best_sq = sq;
            best = i;
        }
    }
    return best;
}

struct DsLabelAlias {
    const char* canonical;
    const char* bs2051;
};

// Non-standard RoomCentric and DAW shorthand labels found in ADM BWF exports.
// The renderer's built-in layouts use BS.2051-style labels; label routing must
// happen before position fallback so LFE channels and bed channels land in the
// intended output slots.
// clang-format off
constexpr std::array<DsLabelAlias, 34> k_ds_aliases = {{
    {"RCL",   "M+030"}, {"RCR",   "M-030"}, {"RCC",   "M+000"},
    {"RCLFE", "LFE1"},  {"RCLSS", "M+090"}, {"RCRSS", "M-090"},
    {"RCLRS", "M+135"}, {"RCRRS", "M-135"},
    {"RCLTS", "U+090"}, {"RCRTS", "U-090"},
    {"L",     "M+030"}, {"R",     "M-030"}, {"C",     "M+000"},
    {"LFE",   "LFE1"},  {"LFEL",  "LFE1"},  {"LFER",  "LFE2"},
    {"LS",    "M+090"}, {"RS",    "M-090"},
    {"LSS",   "M+090"}, {"RSS",   "M-090"},
    {"LRS",   "M+135"}, {"RRS",   "M-135"},
    {"LB",    "M+135"}, {"RB",    "M-135"},
    {"LW",    "M+060"}, {"RW",    "M-060"},
    {"CS",    "M+180"},
    {"VHL",   "U+045"}, {"VHR",   "U-045"}, {"VHC",   "U+000"},
    {"TSL",   "U+090"}, {"TSR",   "U-090"},
    {"LTM",   "U+090"}, {"RTM",   "U-090"},
}};
// clang-format on

[[nodiscard]] std::string canonicalize_ds_label(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (const char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-') {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

[[nodiscard]] std::optional<std::string_view> resolve_ds_alias(std::string_view label) {
    const auto key = canonicalize_ds_label(label);
    const auto* const it =
        std::ranges::find_if(k_ds_aliases, [&](const DsLabelAlias& entry) { return key == entry.canonical; });
    if (it != k_ds_aliases.end()) {
        return it->bs2051;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> speaker_index_for_label(const LayoutSpec& layout, std::string_view label) {
    const auto it = std::ranges::find_if(layout.speakers, [label](const VbapSpeakerSpec& speaker) {
        return !speaker.label.empty() && speaker.label == label;
    });
    if (it == layout.speakers.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(layout.speakers.begin(), it));
}

bool route_one_direct_speaker_label(const LayoutSpec& layout,
                                    std::string_view label,
                                    float gain,
                                    std::vector<float>& gains) {
    if (const auto index = speaker_index_for_label(layout, label)) {
        gains[*index] = gain;
        return true;
    }
    return false;
}

[[nodiscard]] bool route_direct_speaker_label(const LayoutSpec& layout,
                                              const std::vector<std::string>& labels,
                                              float gain,
                                              std::vector<float>& gains) {
    if (std::ranges::any_of(
            labels, [&](const auto& label) { return route_one_direct_speaker_label(layout, label, gain, gains); })) {
        return true;
    }
    return std::ranges::any_of(labels, [&](const auto& label) {
        const auto resolved = resolve_ds_alias(label);
        return resolved && route_one_direct_speaker_label(layout, *resolved, gain, gains);
    });
}

[[nodiscard]] SpeakerDirection source_direction(const SceneBlockPosition& pos) {
    if (!pos.cartesian) {
        return {pos.azimuth, pos.elevation, {}};
    }

    const auto x = static_cast<double>(pos.x);
    const auto y = static_cast<double>(pos.y);
    const auto z = static_cast<double>(pos.z);
    const auto xy = std::hypot(x, y);
    if (xy == 0.0 && z == 0.0) {
        return {};
    }

    constexpr auto rad_to_deg = 180.0 / std::numbers::pi;
    return {static_cast<float>(std::atan2(-x, y) * rad_to_deg), static_cast<float>(std::atan2(z, xy) * rad_to_deg), {}};
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
    auto speakers = flatten_layout(layout); // non-LFE only
    const auto src = source_direction(block.position);
    std::vector<float> source{src.azimuth, src.elevation};

    const auto num_non_lfe = static_cast<int>(speakers.size() / 2U);
    const bool use_3d = !is_2d_layout(layout);
    const float spread_deg = use_3d ? mdap_spread_degrees(block) : 0.0F;
    int table_size = 0;
    int simplex_count = 0;
    float* raw_table = nullptr;

    if (!use_3d) {
        generateVBAPgainTable2D_srcs(
            source.data(), 1, speakers.data(), num_non_lfe, &raw_table, &table_size, &simplex_count);
    } else {
        constexpr int k_omit_large_triangles = 1;
        constexpr int k_enable_dummies = 1;
        generateVBAPgainTable3D_srcs(source.data(),
                                     1,
                                     speakers.data(),
                                     num_non_lfe,
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

    // Expand VBAP gains (non-LFE only) to full output channel count.
    // LFE channels stay zero — Objects never route to LFE.
    std::vector<float> gains(layout.speakers.size(), 0.0F);
    std::size_t vbap_idx = 0;
    for (std::size_t i = 0; i < layout.speakers.size(); ++i) {
        if (!layout.speakers[i].is_lfe) {
            gains[i] = table.get()[vbap_idx++] * block.gain;
        }
    }
    return gains;
}

// Returns true if any non-muted Objects block has non-negligible elevation.
[[nodiscard]] bool scene_has_elevated_sources(const AdmScene& scene) {
    constexpr float k_el_threshold = 1.0e-3F; // 0.001°, well below any real source
    const auto block_has_elevation = [](const SceneObject& obj, const SceneObjectBlock& block) {
        const auto position =
            obj.position_offset ? apply_position_offset(block.position, *obj.position_offset) : block.position;
        const float el = position.cartesian
                             ? static_cast<float>(std::atan2(static_cast<double>(position.z),
                                                             std::hypot(static_cast<double>(position.x),
                                                                        static_cast<double>(position.y))) *
                                                  (180.0 / std::numbers::pi_v<double>) )
                             : position.elevation;
        return std::fabs(el) > k_el_threshold;
    };
    return std::ranges::any_of(scene.objects, [&](const SceneObject& obj) {
        return !obj.mute && std::ranges::any_of(obj.tracks, [&](const SceneTrackRef& track) {
            return std::ranges::any_of(track.blocks,
                                       [&](const SceneObjectBlock& block) { return block_has_elevation(obj, block); });
        });
    });
}

[[nodiscard]] Result<std::vector<ChannelGainInfo>>
build_gain_matrix(const AdmScene& scene, const LayoutSpec& layout, std::string_view layout_id, LogSink& logs) {
    // Warn once if 2D output will silently discard height information.
    if (is_2d_layout(layout) && scene_has_elevated_sources(scene)) {
        logs.log(LogLevel::warning,
                 "saf-vbap",
                 fmt::format("output layout '{}' is 2D but scene contains Objects with non-zero elevation — "
                             "height information will be projected to the horizontal plane",
                             std::string{layout_id}));
    }

    // Accumulate blocks per input channel so we can sort and interpolate.
    std::map<uint16_t, ChannelGainInfo> by_channel;
    const auto num_out = layout.speakers.size();

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

            // Objects blocks → VBAP panning.
            for (const auto& raw_block : track.blocks) {
                SceneObjectBlock block = raw_block;
                if (obj.position_offset) {
                    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                    const auto& off = *obj.position_offset;
                    block.position = apply_position_offset(raw_block.position, off);
                }
                auto gains = calculate_vbap_gains(block, layout);
                if (!gains) {
                    return make_error(
                        gains.error().code, gains.error().message, fmt::format("track_uid={}", track.track_uid));
                }
                if (obj.gain != 1.0F) {
                    std::ranges::transform(*gains, gains->begin(), [g = obj.gain](float v) { return v * g; });
                }
                cg.blocks.push_back({std::move(*gains),
                                     raw_block.start_sample,
                                     std::min(raw_block.end_sample, obj.end_sample),
                                     raw_block.jump_position,
                                     raw_block.interp_length_samples});
            }

            // DirectSpeakers blocks → label match, then nearest-speaker fallback.
            // LFE-identified channels (channelFrequency.lowPass) skip the fallback:
            // routing bass content to a full-range speaker would be incorrect.
            // DS channels are treated as jump_position=true (no interpolation).
            for (const auto& ds : track.ds_blocks) {
                std::vector<float> gains(num_out, 0.0F);

                const bool matched = route_direct_speaker_label(layout, ds.speaker_labels, ds.gain, gains);

                if (!matched) {
                    if (ds.low_pass_hz) {
                        // LFE channel with no matching LFE output: drop rather than misroute.
                        logs.log(LogLevel::warning,
                                 "saf-vbap",
                                 fmt::format("LFE channel (lowPass={:.0f}Hz) has no matching LFE output "
                                             "in layout '{}' — channel dropped",
                                             *ds.low_pass_hz,
                                             std::string{layout_id}));
                    } else {
                        const float az = ds.has_position ? ds.azimuth : 0.0F;
                        const float el = ds.has_position ? ds.elevation : 0.0F;
                        if (!ds.speaker_labels.empty()) {
                            logs.log(LogLevel::warning,
                                     "saf-vbap",
                                     fmt::format("DirectSpeakers label '{}' not in output layout — "
                                                 "routing to nearest speaker",
                                                 ds.speaker_labels.front()));
                        }
                        gains[nearest_speaker_index(layout, az, el)] = ds.gain;
                    }
                }

                if (obj.gain != 1.0F) {
                    std::ranges::transform(gains, gains.begin(), [g = obj.gain](float v) { return v * g; });
                }
                cg.blocks.push_back(
                    {std::move(gains), ds.start_sample, std::min(ds.end_sample, obj.end_sample), true, std::nullopt});
            }
        }
    }

    // Sort each channel's blocks by start_sample for sequential access.
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

[[nodiscard]] float interpolated_gain(
    const BlockGains& previous, const BlockGains& current, std::size_t out_ch, uint64_t delta, uint64_t interp_len) {
    const float alpha = static_cast<float>(delta) / static_cast<float>(interp_len);
    return (previous.gains[out_ch] * (1.0F - alpha)) + (current.gains[out_ch] * alpha);
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
        float gain = block.gains[out_ch];
        if (ramping) {
            gain = interpolated_gain(channel.blocks[block_index - 1], block, out_ch, delta, interp_len);
        }
        (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += in_sample * gain;
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

class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<void> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

Result<void> VbapRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    const std::string layout_id = plan.output_layout;
    const auto layout = layout_spec(layout_id);
    if (!layout.has_value()) {
        return make_error(ErrorCode::unsupported, fmt::format("unsupported VBAP output layout '{}'", layout_id), {});
    }

    const auto& info = plan.scene.info;

    auto gain_matrix = build_gain_matrix(plan.scene, *layout, layout_id, logs);
    if (!gain_matrix) {
        return make_error(gain_matrix.error().code, gain_matrix.error().message, gain_matrix.error().context);
    }
    if (gain_matrix->empty()) {
        logs.log(LogLevel::warning, "saf-vbap", "no renderable tracks found (all muted?), writing silence");
    }

    const auto num_out_ch = static_cast<uint16_t>(layout->speakers.size());
    const auto num_in_ch = info.num_channels;
    const auto num_frames = info.num_frames;
    const auto sample_rate = info.sample_rate;

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
        logs.log(LogLevel::info,
                 "saf-vbap",
                 fmt::format("rendering {} tracks (Objects + DirectSpeakers) → {} channels, {} frames [{}]",
                             gain_matrix->size(),
                             num_out_ch,
                             num_frames,
                             is_2d_layout(*layout) ? "2D VBAP" : "3D VBAP"));
        progress.on_progress({RenderStage::rendering, 0.3, "rendering audio"});

        auto reader = bw64::readFile(plan.input_path);
        auto writer_res = audio::WriterHandle::open(
            plan.output_path, num_out_ch, static_cast<uint32_t>(sample_rate), plan.output_layout);
        if (!writer_res) {
            return tl::unexpected{writer_res.error()};
        }
        auto& writer = *writer_res;

        const uint64_t k_default_interp = static_cast<uint64_t>(sample_rate) * plan.default_interp_ms / 1000;

        // Current block index per channel — advanced monotonically as frames_done increases.
        std::vector<std::size_t> blk_idx(gain_matrix->size(), 0);

        constexpr uint64_t k_block_size = 1024;
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
        std::vector<float> out_block(static_cast<std::size_t>(num_out_ch) * k_block_size);
        uint64_t frames_done = 0;

        while (frames_done < num_frames) {
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            const AccumulateContext ctx{
                in_block.data(), &out_block, frames_done, num_in_ch, num_out_ch, k_default_interp};
            accumulate_gain_matrix(*gain_matrix, blk_idx, ctx, frames_now);

            if (writer.write(out_block.data(), frames_now) != frames_now) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
            }
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

bool register_vbap_layout(std::string id, std::string display_name, std::vector<VbapSpeakerSpec> speakers) {
    if (id.empty() || speakers.empty()) {
        return false;
    }
    if (std::ranges::all_of(speakers, [](const auto& s) { return s.is_lfe; })) {
        return false; // nothing to pan
    }
    if (std::ranges::any_of(speakers,
                            [](const auto& s) { return !std::isfinite(s.azimuth) || !std::isfinite(s.elevation); })) {
        return false;
    }
    auto& reg = layout_registry();
    if (std::ranges::any_of(reg, [&id](const RegistryEntry& e) { return e.id == id; })) {
        return false;
    }
    reg.push_back({std::move(id), std::move(display_name), std::move(speakers)});
    return true;
}

CapabilityReport vbap_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-vbap";
    r.backend_version = "1.3.4";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;

    for (const auto& entry : layout_registry()) {
        CapabilityReport::Layout layout;
        layout.id = entry.id;
        layout.display_name = entry.display_name;
        layout.channel_count = static_cast<uint16_t>(entry.speakers.size());
        layout.lfe_count =
            static_cast<uint16_t>(std::ranges::count_if(entry.speakers, [](const auto& s) { return s.is_lfe; }));
        layout.is_3d = std::ranges::any_of(entry.speakers,
                                           [](const auto& s) { return !s.is_lfe && std::fabs(s.elevation) > 1.0e-6F; });
        layout.supports_spread = layout.is_3d; // 2D VBAP passes spread=0; MDAP/SAF only helps for 3D
        r.supported_layouts.push_back(std::move(layout));
    }
    return r;
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
