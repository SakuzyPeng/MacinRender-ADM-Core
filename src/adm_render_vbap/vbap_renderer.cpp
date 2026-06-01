#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ebur128.h>
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

#include "render_common.h"
#include "speaker_layouts.h"

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

std::vector<RegistryEntry>& custom_layout_registry() {
    static std::vector<RegistryEntry> reg;
    return reg;
}

[[nodiscard]] VbapSpeakerSpec to_vbap_speaker(const render_layouts::SpeakerSpec& speaker) {
    return {speaker.azimuth, speaker.elevation, std::string{speaker.label}, speaker.is_lfe};
}

[[nodiscard]] LayoutSpec layout_spec_from_shared(const render_layouts::SpeakerLayout& layout) {
    LayoutSpec spec;
    spec.speakers.reserve(layout.speakers.size());
    std::ranges::transform(layout.speakers, std::back_inserter(spec.speakers), to_vbap_speaker);
    return spec;
}

struct BlockGains {
    std::vector<float> gains;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    bool jump_position{false};
    bool smoothable_object{false};
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
    uint64_t object_smoothing_frames{0};
};

struct SafFree {
    // SAF allocates gain tables through its C API; ownership transfers to caller.
    void operator()(float* ptr) const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        std::free(ptr);
    }
};

[[nodiscard]] std::optional<LayoutSpec> layout_spec(std::string_view layout_id) {
    if (const auto* shared = render_layouts::find_speaker_layout(layout_id); shared != nullptr) {
        return layout_spec_from_shared(*shared);
    }
    const auto& reg = custom_layout_registry();
    const auto it = std::ranges::find_if(reg, [layout_id](const RegistryEntry& e) { return e.id == layout_id; });
    if (it != reg.end()) {
        return LayoutSpec{it->speakers};
    }
    return std::nullopt;
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

[[nodiscard]] std::vector<SceneOutputSpeaker> output_speakers(const LayoutSpec& layout) {
    std::vector<SceneOutputSpeaker> result;
    result.reserve(layout.speakers.size());
    std::ranges::transform(layout.speakers, std::back_inserter(result), [](const VbapSpeakerSpec& speaker) {
        return SceneOutputSpeaker{speaker.azimuth, speaker.elevation, speaker.is_lfe};
    });
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
    const auto polar = scene_position_to_polar(pos);
    return {polar.azimuth, polar.elevation, {}};
}

// Map ADM Objects extent parameters to a MDAP spread angle in degrees.
// Port of ADMVBAPMDAPSpreadDegreesForExtent from the ObjC renderer.
// 2D layouts pass spread=0 to the SAF API (2D VBAP has no spread parameter).
[[nodiscard]] float mdap_spread_degrees(const SceneObjectBlock& block) {
    const float distance = block.position.cartesian ? std::hypot(block.position.x, block.position.y, block.position.z)
                                                    : block.position.distance;
    const float spread_scale = std::clamp(1.0F / std::max(0.4F, distance), 0.5F, 2.5F);
    const float w = std::max(0.0F, block.width) * 60.0F * spread_scale;
    const float w_with_divergence =
        block.divergence > 1.0e-4F ? std::max(w, std::max(0.0F, block.divergence_azimuth_range) * 0.5F) : w;
    const float h = std::max(0.0F, block.height) * 45.0F * spread_scale;
    const float d = std::max(0.0F, block.depth) * 20.0F * spread_scale;
    return std::min(180.0F, std::hypot(w_with_divergence, h, d));
}

[[nodiscard]] Result<std::vector<float>> calculate_one_vbap_gains(const SceneObjectBlock& block,
                                                                  const LayoutSpec& layout,
                                                                  mradm::SpeakerSpreadMode spread_mode) {
    auto speakers = flatten_layout(layout); // non-LFE only
    const auto src = source_direction(block.position);
    std::vector<float> source{src.azimuth, src.elevation};

    const auto num_non_lfe = static_cast<int>(speakers.size() / 2U);
    const bool use_3d = !is_2d_layout(layout);
    float spread_deg = 0.0F;
    if (use_3d) {
        switch (spread_mode) {
        case mradm::SpeakerSpreadMode::none:
            spread_deg = 0.0F;
            break;
        case mradm::SpeakerSpreadMode::automatic:
        case mradm::SpeakerSpreadMode::mdap:
            spread_deg = mdap_spread_degrees(block);
            break;
        }
    }
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

[[nodiscard]] Result<std::vector<ChannelGainInfo>> build_gain_matrix(const AdmScene& scene,
                                                                     const LayoutSpec& layout,
                                                                     std::string_view layout_id,
                                                                     LogSink& logs,
                                                                     mradm::SpeakerSpreadMode spread_mode) {
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
    const auto object_speakers = output_speakers(layout);
    bool screen_ref_warned{false};

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
                const auto prepared = render_common::prepare_object_block(
                    raw_block, obj, object_speakers, logs, "saf-vbap", screen_ref_warned);
                std::vector<float> gains(num_out, 0.0F);
                for (const auto& source : prepared.sources) {
                    auto source_gains = calculate_one_vbap_gains(source, layout, spread_mode);
                    if (!source_gains) {
                        return make_error(source_gains.error().code,
                                          source_gains.error().message,
                                          fmt::format("track_uid={}", track.track_uid));
                    }
                    for (std::size_t i = 0; i < gains.size(); ++i) {
                        gains[i] += (*source_gains)[i];
                    }
                }
                if (obj.gain != 1.0F) {
                    std::ranges::transform(gains, gains.begin(), [g = obj.gain](float v) { return v * g; });
                }
                cg.blocks.push_back({std::move(gains),
                                     prepared.start_sample,
                                     prepared.end_sample,
                                     prepared.jump_position,
                                     true,
                                     prepared.interp_length_samples});
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
                cg.blocks.push_back({std::move(gains),
                                     ds.start_sample,
                                     std::min(ds.end_sample, obj.end_sample),
                                     true,
                                     false,
                                     std::nullopt});
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
    const uint64_t interp_len = render_common::interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;

    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        float gain = block.gains[out_ch];
        if (ramping) {
            gain = render_common::interpolated_scalar(
                channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len);
        }
        (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += in_sample * gain;
    }
}

[[nodiscard]] bool gains_at_frame(const ChannelGainInfo& channel,
                                  std::size_t& block_index,
                                  const AccumulateContext& ctx,
                                  uint64_t abs_frame,
                                  std::vector<float>& gains) {
    std::ranges::fill(gains, 0.0F);
    while (block_index + 1 < channel.blocks.size() && abs_frame >= channel.blocks[block_index + 1].start_sample) {
        ++block_index;
    }

    const auto& block = channel.blocks[block_index];
    if (abs_frame < block.start_sample || abs_frame >= block.end_sample) {
        return false;
    }

    const uint64_t interp_len = render_common::interpolation_length(channel, block_index, ctx.default_interp);
    const uint64_t delta = abs_frame - block.start_sample;
    const bool ramping = interp_len > 0 && delta < interp_len;
    for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
        gains[out_ch] = block.gains[out_ch];
        if (ramping) {
            gains[out_ch] = render_common::interpolated_scalar(
                channel.blocks[block_index - 1].gains[out_ch], block.gains[out_ch], delta, interp_len);
        }
    }
    return block.smoothable_object;
}

void accumulate_gain_matrix(const std::vector<ChannelGainInfo>& gain_matrix,
                            std::vector<std::size_t>& block_indices,
                            const AccumulateContext& ctx,
                            uint64_t frames_now) {
    std::vector<float> start_gains(ctx.num_out_ch);
    std::vector<float> end_gains(ctx.num_out_ch);
    for (std::size_t ci = 0; ci < gain_matrix.size(); ++ci) {
        const auto& channel = gain_matrix[ci];
        if (channel.blocks.empty()) {
            continue;
        }
        auto start_index = block_indices[ci];
        auto end_index = start_index;
        const bool smooth_start =
            ctx.object_smoothing_frames > 0 && gains_at_frame(channel, start_index, ctx, ctx.frames_done, start_gains);
        const bool smooth_end = ctx.object_smoothing_frames > 0 &&
                                gains_at_frame(channel, end_index, ctx, ctx.frames_done + frames_now - 1, end_gains);
        if (!smooth_start || !smooth_end) {
            for (std::size_t frame = 0; frame < frames_now; ++frame) {
                accumulate_channel_block(channel, block_indices[ci], ctx, frame);
            }
            continue;
        }
        block_indices[ci] = start_index;

        for (std::size_t frame = 0; frame < frames_now; ++frame) {
            const float alpha = frames_now > 1 ? static_cast<float>(frame) / static_cast<float>(frames_now - 1) : 0.0F;
            const float in_sample = ctx.input[(frame * ctx.num_in_ch) + channel.input_channel];
            for (std::size_t out_ch = 0; out_ch < ctx.num_out_ch; ++out_ch) {
                const float gain = (start_gains[out_ch] * (1.0F - alpha)) + (end_gains[out_ch] * alpha);
                (*ctx.output)[(frame * ctx.num_out_ch) + out_ch] += in_sample * gain;
            }
        }
    }
}

class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<RenderMetrics> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> VbapRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    const std::string layout_id = plan.output_layout;
    const auto layout = layout_spec(layout_id);
    if (!layout.has_value()) {
        return make_error(ErrorCode::unsupported, fmt::format("unsupported VBAP output layout '{}'", layout_id), {});
    }

    const auto& info = plan.scene.info;

    auto gain_matrix = build_gain_matrix(plan.scene, *layout, layout_id, logs, plan.speaker_spread_mode);
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

        struct EburFree {
            void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
        };
        using EburPtr = std::unique_ptr<ebur128_state, EburFree>;
        EburPtr lufs_st{
            ebur128_init(num_out_ch, static_cast<unsigned long>(sample_rate), EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};

        constexpr uint64_t k_min_block_size = 1024;
        const uint64_t k_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);
        std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);

        // Loudness / true-peak measurement dominates this renderer (≈70% at 22.2). Run it on a
        // background thread so it overlaps with the next block's read + mix. Double-buffer the output
        // so the next block can be mixed while the meter still reads the previous one; reuse of a
        // buffer waits on its outstanding measurement future. Block order is preserved on the worker,
        // so the measured loudness / true peak is identical to the inline version.
        constexpr std::size_t k_num_buffers = 2;
        std::array<std::vector<float>, k_num_buffers> out_buffers;
        for (auto& buffer : out_buffers) {
            buffer.assign(static_cast<std::size_t>(num_out_ch) * k_block_size, 0.0F);
        }
        std::array<std::future<void>, k_num_buffers> meter_pending;
        render_common::SerialWorker meter;
        std::size_t buf_idx = 0;

        // On-demand output window (RenderPlan::render_window). VBAP has no DSP state,
        // but object smoothing samples gains at block edges, so windowed rendering
        // still starts at the full-render block boundary containing win_start. Frames
        // before win_start are processed for identical smoothing segmentation but not
        // written. When not windowed, win_start=0 / win_end=num_frames reproduces the
        // full render.
        const bool windowed = plan.render_window.has_value();
        const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
        const uint64_t win_end =
            windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
        const uint64_t start_pos = windowed ? (win_start / k_block_size) * k_block_size : 0;
        if (start_pos > 0) {
            render_common::seek_reader_abs(*reader, start_pos);
        }
        const double progress_span = static_cast<double>(std::max<uint64_t>(1, win_end - start_pos));
        uint64_t frames_done = start_pos;

        while (frames_done < win_end) {
            if (plan.cancel_token.stop_requested()) {
                return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
            }
            const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
            const std::size_t out_samples = static_cast<std::size_t>(num_out_ch) * frames_now;

            // Reclaim this buffer once the meter has finished its previous use of it.
            if (meter_pending.at(buf_idx).valid()) {
                meter_pending.at(buf_idx).get();
            }
            std::vector<float>& out_block = out_buffers.at(buf_idx);

            reader->read(in_block.data(), frames_now);
            std::fill(out_block.begin(), out_block.begin() + static_cast<ptrdiff_t>(out_samples), 0.0F);

            const AccumulateContext ctx{in_block.data(),
                                        &out_block,
                                        frames_done,
                                        num_in_ch,
                                        num_out_ch,
                                        k_default_interp,
                                        plan.object_smoothing_frames};
            accumulate_gain_matrix(*gain_matrix, blk_idx, ctx, frames_now);

            // Sub-range of this block inside the output window [win_start, win_end).
            const uint64_t w_lo = std::max(frames_done, win_start);
            const uint64_t w_hi = std::min(frames_done + frames_now, win_end);
            const bool emit = w_hi > w_lo;
            const std::size_t emit_off = emit ? static_cast<std::size_t>(w_lo - frames_done) : 0;
            const std::size_t emit_count = emit ? static_cast<std::size_t>(w_hi - w_lo) : 0;

            if (emit && writer.write(out_block.data() + (emit_off * num_out_ch), emit_count) != emit_count) {
                return make_error(ErrorCode::io_error, "short write while rendering", "output=" + plan.output_path);
            }

            // Meter the written frames. Windowed → exactly emit_count; otherwise honor meter_window.
            if (lufs_st) {
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
                    ebur128_state* state = lufs_st.get();
                    const float* data = out_block.data() + (meter_off * num_out_ch);
                    const auto frame_count = meter_count;
                    meter_pending.at(buf_idx) =
                        meter.post([state, data, frame_count] { ebur128_add_frames_float(state, data, frame_count); });
                }
            }

            frames_done += frames_now;
            buf_idx = (buf_idx + 1) % k_num_buffers;

            const uint64_t progress_done = std::min(frames_done, win_end) - start_pos;
            const double frac = 0.3 + (0.6 * (static_cast<double>(progress_done) / progress_span));
            progress.on_progress({RenderStage::rendering, frac, "rendering"});
        }

        // All audio is written; wait for outstanding measurements before querying global metrics.
        for (auto& pending : meter_pending) {
            if (pending.valid()) {
                pending.get();
            }
        }

        progress.on_progress({RenderStage::finished, 1.0, "done"});
        logs.log(LogLevel::info,
                 "saf-vbap",
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
            for (unsigned int ch = 0; ch < num_out_ch; ++ch) {
                double ch_peak = 0.0;
                if (ebur128_true_peak(lufs_st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
                    max_peak = std::max(max_peak, ch_peak);
                }
            }
            if (max_peak > 0.0) {
                metrics.measured_peak_dbtp = 20.0 * std::log10(max_peak);
            }
        }
        return metrics;
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
    if (render_layouts::find_speaker_layout(id) != nullptr) {
        return false;
    }
    auto& reg = custom_layout_registry();
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
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_screen_ref = false;
    r.supports_diffuse = false;
    r.supports_render_window = true; // direct seek; no DSP state, no pre-roll needed

    auto append_layout = [&](std::string_view id, std::string_view display_name, const auto& speakers) {
        CapabilityReport::Layout layout;
        layout.id = std::string{id};
        layout.display_name = std::string{display_name};
        layout.channel_count = static_cast<uint16_t>(speakers.size());
        layout.lfe_count =
            static_cast<uint16_t>(std::ranges::count_if(speakers, [](const auto& s) { return s.is_lfe; }));
        layout.is_3d =
            std::ranges::any_of(speakers, [](const auto& s) { return !s.is_lfe && std::fabs(s.elevation) > 1.0e-6F; });
        layout.supports_spread = layout.is_3d; // 2D VBAP passes spread=0; MDAP/SAF only helps for 3D
        r.supported_layouts.push_back(std::move(layout));
    };

    for (const auto& entry : render_layouts::speaker_layouts()) {
        append_layout(entry.id, entry.display_name, entry.speakers);
    }
    for (const auto& entry : custom_layout_registry()) {
        append_layout(entry.id, entry.display_name, entry.speakers);
    }
    return r;
}

std::unique_ptr<IRenderer> create_vbap_renderer() {
    return std::make_unique<VbapRenderer>();
}

} // namespace mradm
