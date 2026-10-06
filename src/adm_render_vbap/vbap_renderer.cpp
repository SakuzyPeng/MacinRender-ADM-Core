#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_vbap.h"

#include "consistency_trace.h"
#include "dsp.h"
#include "render_common.h"
#include "scene_math.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

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

using render_common::BlockGains;
using render_common::ChannelGainInfo;


[[nodiscard]] std::optional<LayoutSpec> layout_spec(std::string_view layout_id, SpeakerGeometry geometry) {
    if (const auto* shared = render_layouts::find_speaker_layout(layout_id, geometry); shared != nullptr) {
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

[[nodiscard]] std::vector<render_common::DirectSpeakerRoutingTarget> direct_speaker_targets(const LayoutSpec& layout) {
    std::vector<render_common::DirectSpeakerRoutingTarget> targets;
    targets.reserve(layout.speakers.size());
    std::ranges::transform(layout.speakers, std::back_inserter(targets), [](const VbapSpeakerSpec& speaker) {
        return render_common::DirectSpeakerRoutingTarget{
            speaker.label, speaker.azimuth, speaker.elevation, speaker.is_lfe};
    });
    return targets;
}

[[nodiscard]] SpeakerDirection source_direction(const SceneBlockPosition& pos) {
    const auto polar = scene_position_to_polar(pos);
    return {polar.azimuth, polar.elevation, {}};
}

// Map ADM Objects extent parameters to a MDAP spread angle in degrees.
// Port of ADMVBAPMDAPSpreadDegreesForExtent from the ObjC renderer.
// 2D layouts pass spread=0 to the SAF API (2D VBAP has no spread parameter).
[[nodiscard]] float mdap_spread_degrees(const SceneObjectBlock& block) {
    const float distance =
        block.position.cartesian
            ? render_common::canonical_vector_length(block.position.x, block.position.y, block.position.z)
            : block.position.distance;
    return dsp::scene_math<6, 1>(
        6U, {block.width, block.height, block.depth, distance, block.divergence, block.divergence_azimuth_range})[0];
}

[[nodiscard]] Result<std::vector<float>>
calculate_point_vbap_gains(float azimuth, float elevation, float gain, float spread_deg, const LayoutSpec& layout) {
    auto speakers = flatten_layout(layout); // non-LFE only

    const bool use_3d = !is_2d_layout(layout);
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    consistency::dump("vbap.01-source.f32", {azimuth, elevation, gain, spread_deg});
    consistency::dump("vbap.02-speakers.f32", speakers);
#endif
    std::vector<float> table;
    try {
        table = dsp::panner_for(speakers, use_3d)->gains(azimuth, elevation, spread_deg);
    } catch (const std::exception& error) {
        return make_error(ErrorCode::render_failed, error.what());
    }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    consistency::dump("vbap.03-gains.f32", std::span<const float>(table));
#endif

    // Expand VBAP gains (non-LFE only) to full output channel count.
    // LFE channels stay zero — Objects never route to LFE.
    std::vector<float> gains(layout.speakers.size(), 0.0F);
    std::size_t vbap_idx = 0;
    for (std::size_t i = 0; i < layout.speakers.size(); ++i) {
        if (!layout.speakers[i].is_lfe) {
            gains[i] = table[vbap_idx++] * gain;
        }
    }
    return gains;
}

[[nodiscard]] Result<std::vector<float>> calculate_one_vbap_gains(const SceneObjectBlock& block,
                                                                  const LayoutSpec& layout,
                                                                  mradm::SpeakerSpreadMode spread_mode) {
    const auto src = source_direction(block.position);
    float spread_deg = 0.0F;
    if (!is_2d_layout(layout) && spread_mode != mradm::SpeakerSpreadMode::none) {
        spread_deg = mdap_spread_degrees(block);
    }
    return calculate_point_vbap_gains(src.azimuth, src.elevation, block.gain, spread_deg, layout);
}

// Returns true if any non-muted Objects block has non-negligible elevation.
[[nodiscard]] bool scene_has_elevated_sources(const AdmScene& scene) {
    constexpr float k_el_threshold = 1.0e-3F; // 0.001°, well below any real source
    const auto block_has_elevation = [](const SceneObject& obj, const SceneObjectBlock& block) {
        const auto position =
            obj.position_offset ? apply_position_offset(block.position, *obj.position_offset) : block.position;
        const float el = scene_position_to_polar(position).elevation;
        return std::fabs(el) > k_el_threshold;
    };
    return std::ranges::any_of(scene.objects, [&](const SceneObject& obj) {
        return !obj.mute && std::ranges::any_of(obj.tracks, [&](const SceneTrackRef& track) {
            return std::ranges::any_of(track.blocks,
                                       [&](const SceneObjectBlock& block) { return block_has_elevation(obj, block); });
        });
    });
}

// NOLINTNEXTLINE(readability-function-size): linear scene-to-gain-table preparation keeps block precedence visible.
[[nodiscard]] Result<std::vector<ChannelGainInfo>> build_gain_matrix(const AdmScene& scene,
                                                                     const LayoutSpec& layout,
                                                                     std::string_view layout_id,
                                                                     LogSink& logs,
                                                                     mradm::SpeakerSpreadMode spread_mode,
                                                                     DirectSpeakersRoutingMode routing_mode,
                                                                     const DirectSpeakersMatrix* matrix,
                                                                     const render_common::LfeRoutingPlan& lfe_routing) {
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
    const auto routing_targets = direct_speaker_targets(layout);
    std::optional<render_common::ResolvedDirectSpeakersMatrix> resolved_matrix;
    if (routing_mode == DirectSpeakersRoutingMode::matrix) {
        if (matrix == nullptr) {
            return make_error(ErrorCode::invalid_argument, "DirectSpeakers matrix routing requires a parsed matrix");
        }
        auto resolved = render_common::resolve_direct_speakers_matrix_targets(*matrix, routing_targets, layout_id);
        if (!resolved) {
            return tl::unexpected{resolved.error()};
        }
        resolved_matrix = std::move(*resolved);
    }
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
            cg.object_id = obj.id;
            // Capture the channel's DirectSpeakers label so a per-channel live override can target
            // one bed channel (Objects tracks have no ds_blocks → key stays empty → whole-object).
            if (!track.ds_blocks.empty() && !track.ds_blocks.front().speaker_labels.empty()) {
                cg.speaker_label_key =
                    render_common::canonicalise_speaker_label(track.ds_blocks.front().speaker_labels.front());
            }

            // Objects blocks use SAF VBAP/MDAP.
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


            // DirectSpeakers blocks use the selected label or position path. LFE
            // identification always takes precedence and retains dedicated routing.
            // DS channels are treated as jump_position=true (no interpolation).
            for (const auto& ds : track.ds_blocks) {
                std::vector<float> gains(num_out, 0.0F);

                const auto lfe_target = render_common::direct_speakers_lfe_target(ds);
                if (lfe_routing.applies_to_22_2 && lfe_target != render_common::LfeTarget::none) {
                    gains[render_common::k_22_2_lfe1_index] =
                        ds.gain * lfe_routing.gain(lfe_target, render_common::LfeTarget::lfe1);
                    gains[render_common::k_22_2_lfe2_index] =
                        ds.gain * lfe_routing.gain(lfe_target, render_common::LfeTarget::lfe2);
                } else if (lfe_target != render_common::LfeTarget::none) {
                    const auto target =
                        render_common::direct_speaker_index_for_labels(routing_targets, ds.speaker_labels);
                    if (target && routing_targets[*target].is_lfe) {
                        gains[*target] = ds.gain;
                    } else {
                        logs.log(LogLevel::warning,
                                 "saf-vbap",
                                 fmt::format("DirectSpeakers LFE channel has no matching LFE output in layout '{}' "
                                             "— channel dropped",
                                             std::string{layout_id}));
                    }
                } else if (routing_mode == DirectSpeakersRoutingMode::matrix) {
                    if (!resolved_matrix) {
                        return make_error(ErrorCode::render_failed, "resolved DirectSpeakers matrix is missing");
                    }
                    auto route = render_common::direct_speakers_matrix_route_for_block(*resolved_matrix, ds);
                    if (!route) {
                        return tl::unexpected{route.error()};
                    }
                    for (const auto& target : (*route)->targets) {
                        gains[target.output_channel] = ds.gain * target.gain;
                    }
                } else if (routing_mode == DirectSpeakersRoutingMode::position) {
                    const auto position = render_common::direct_speaker_position_or_front(ds, logs, "saf-vbap");
                    auto position_gains =
                        calculate_point_vbap_gains(position.azimuth, position.elevation, ds.gain, 0.0F, layout);
                    if (!position_gains) {
                        return make_error(position_gains.error().code,
                                          position_gains.error().message,
                                          fmt::format("track_uid={}", track.track_uid));
                    }
                    gains = std::move(*position_gains);
                } else {
                    const auto target =
                        render_common::direct_speaker_index_for_labels(routing_targets, ds.speaker_labels);
                    if (target && !routing_targets[*target].is_lfe) {
                        gains[*target] = ds.gain;
                    } else {
                        const auto label_position =
                            render_common::direct_speaker_position_for_labels(ds.speaker_labels);
                        const auto position =
                            label_position ? *label_position
                                           : render_common::direct_speaker_position_or_front(ds, logs, "saf-vbap");
                        const std::string label =
                            ds.speaker_labels.empty() ? std::string{"<missing>"} : ds.speaker_labels.front();
                        logs.log(LogLevel::warning,
                                 "saf-vbap",
                                 fmt::format("DirectSpeakers label '{}' not in output layout — spatializing from {}",
                                             label,
                                             label_position ? "label direction" : "nominal position"));
                        auto fallback_gains =
                            calculate_point_vbap_gains(position.azimuth, position.elevation, ds.gain, 0.0F, layout);
                        if (!fallback_gains) {
                            return make_error(fallback_gains.error().code,
                                              fallback_gains.error().message,
                                              fmt::format("track_uid={}", track.track_uid));
                        }
                        gains = std::move(*fallback_gains);
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

// Immutable, reusable VBAP state: the resolved layout and the SAF VBAP gain matrix
// (the expensive per-object gain-table computation). Reused across render_window()
// calls (PreviewSession scrubbing); no per-output state.
struct VbapPrepared final : IPreparedRender {
    LayoutSpec layout;
    render_common::PreparedPcmMix gain_matrix;
};

// Realtime streaming VBAP session over the same prepared gain matrix as render_window.
// VBAP carries no DSP state across blocks (just a monotonic per-channel block cursor), so
// streaming is a thin loop: render k_block_size-aligned blocks via the SAME
// Rust timeline mixer the offline path uses, into a FIFO that process() serves at any
// requested frame count — bit-identical to render_window for a gap-free run from frame 0.
// seek() resets the per-channel block cursors (the accumulators re-find the right block)
// and repositions the reader. set_overrides applies live per-object gain by pre-scaling the
// matching input channels before the (linear) VBAP mix — equivalent to scaling the object
// gain; topology scales (diffuse/extent/divergence) are not applicable to VBAP and ignored.
class VbapStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<VbapStream>>
    create(const VbapPrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        auto reader = audio::RenderInputReader::open(plan.input_path);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        (void) logs;
        return std::unique_ptr<VbapStream>{new VbapStream(prepared, std::move(*reader), plan)};
    }

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (fifo_read_ >= fifo_.size()) {
                if (frames_done_ >= total_frames_) {
                    break;
                }
                if (auto result = render_block(); !result) {
                    return tl::unexpected{result.error()};
                }
                if (fifo_read_ >= fifo_.size()) {
                    break;
                }
            }
            const std::size_t avail = (fifo_.size() - fifo_read_) / num_out_ch_;
            const std::size_t take = std::min(frames - produced, avail);
            std::copy_n(fifo_.data() + fifo_read_, take * num_out_ch_, out.data() + (produced * num_out_ch_));
            fifo_read_ += take * num_out_ch_;
            produced += take;
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        mix_.reset();
        frames_done_ = std::min(frame, total_frames_);
        if (auto result = reader_->seek_frame(frames_done_); !result) {
            return tl::unexpected{result.error()};
        }
        fifo_.clear();
        fifo_read_ = 0;
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        std::ranges::fill(live_gain_targets_, 1.0F);
        // The override is projected object → input channel (each channel scaled by its owning
        // object's gain). A per-channel override (DirectSpeakers speaker_label) targets one bed
        // channel; an empty-label override scales every channel of the object (whole-object).
        for (const auto& cg : prepared_.gain_matrix) {
            if (cg.input_channel >= live_gain_targets_.size()) {
                continue;
            }
            if (const auto g =
                    render_common::resolve_live_channel_gain(overrides, cg.object_id, cg.speaker_label_key)) {
                live_gain_targets_[cg.input_channel] = *g;
            }
        }
        live_gain_smoother_.set_targets(live_gain_targets_);
    }

    [[nodiscard]] uint32_t out_channels() const override { return num_out_ch_; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return output_layout_; }

  private:
    VbapStream(const VbapPrepared& prepared, std::unique_ptr<audio::RenderInputReader> reader, const RenderPlan& plan)
        : prepared_(prepared), reader_(std::move(reader)), num_in_ch_(plan.scene.info.num_channels),
          num_out_ch_(static_cast<uint16_t>(prepared.layout.speakers.size())),
          sample_rate_(plan.scene.info.sample_rate), total_frames_(plan.scene.info.num_frames),
          output_layout_(plan.output_layout), object_smoothing_frames_(plan.object_smoothing_frames),
          k_block_size_(std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          mix_(prepared.gain_matrix.plan,
               k_block_size_,
               static_cast<uint64_t>(plan.scene.info.sample_rate) * plan.default_interp_ms / 1000U,
               object_smoothing_frames_ > 0),
          in_block_(static_cast<std::size_t>(plan.scene.info.num_channels) *
                    std::max<uint64_t>(1024U, plan.object_smoothing_frames)),
          live_gain_targets_(plan.scene.info.num_channels, 1.0F),
          live_gain_smoother_(plan.scene.info.num_channels, plan.scene.info.sample_rate) {}

    // Pre-scale the matching input channels by their object's live gain (linear, so this
    // equals scaling the object gain; the VBAP mix downstream is linear).
    void apply_live_gain(uint64_t frames_now) {
        live_gain_smoother_.apply(in_block_.data(), static_cast<std::size_t>(frames_now));
    }

    Result<void> render_block() {
        const uint64_t frames_now = std::min<uint64_t>(k_block_size_, total_frames_ - frames_done_);
        const auto read_result = reader_->read(in_block_.data(), frames_now);
        if (!read_result) {
            return tl::unexpected{read_result.error()};
        }
        if (*read_result != frames_now) {
            return make_error(ErrorCode::io_error, "short render input read");
        }
        apply_live_gain(frames_now);
        fifo_.assign(static_cast<std::size_t>(num_out_ch_) * frames_now, 0.0F);
        fifo_read_ = 0;
        mix_.speaker(std::span{in_block_}.first(static_cast<std::size_t>(frames_now) * num_in_ch_),
                     fifo_,
                     {},
                     frames_done_,
                     frames_now);
        frames_done_ += frames_now;
        return {};
    }

    const VbapPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<audio::RenderInputReader> reader_;
    uint16_t num_in_ch_;
    uint16_t num_out_ch_;
    uint32_t sample_rate_;
    uint64_t total_frames_;
    std::string output_layout_;
    uint64_t object_smoothing_frames_;
    uint64_t k_block_size_;
    dsp::PcmMixer mix_;
    std::vector<float> in_block_;
    std::vector<float> live_gain_targets_; // per-input-channel target multiplier (1.0 = neutral)
    render_common::InterleavedLiveGainSmoother live_gain_smoother_;
    std::vector<float> fifo_;
    std::size_t fifo_read_{0};
    uint64_t frames_done_{0};
};

class VbapRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const VbapPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "saf-vbap: open_stream received an incompatible prepared state", {});
        }
        auto stream = VbapStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

CapabilityReport VbapRenderer::capabilities() const {
    return vbap_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> VbapRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    const std::string layout_id = plan.output_layout;
    auto layout = layout_spec(layout_id, plan.speaker_geometry);
    if (!layout.has_value()) {
        const std::string_view geometry = plan.speaker_geometry == SpeakerGeometry::apple ? "apple" : "standard";
        return make_error(
            ErrorCode::unsupported,
            fmt::format("unsupported VBAP output layout '{}' for speaker geometry '{}'", layout_id, geometry),
            {});
    }
    logs.log(
        LogLevel::info,
        "saf-vbap",
        fmt::format("speaker geometry: {}", plan.speaker_geometry == SpeakerGeometry::apple ? "apple" : "standard"));

    const auto routing_mode = plan.direct_speakers_routing_mode == DirectSpeakersRoutingMode::automatic
                                  ? DirectSpeakersRoutingMode::label
                                  : plan.direct_speakers_routing_mode;
    std::string routing_description{render_common::direct_speakers_routing_mode_name(routing_mode)};
    logs.log(LogLevel::info, "saf-vbap", fmt::format("DirectSpeakers routing: {}", routing_description));

    auto lfe_routing = render_common::resolve_lfe_routing(plan, logs, "saf-vbap");
    if (!lfe_routing) {
        return tl::unexpected{lfe_routing.error()};
    }
    auto gain_matrix = build_gain_matrix(plan.scene,
                                         *layout,
                                         layout_id,
                                         logs,
                                         plan.speaker_spread_mode,
                                         routing_mode,
                                         plan.direct_speakers_matrix.get(),
                                         *lfe_routing);
    if (!gain_matrix) {
        return tl::unexpected{gain_matrix.error()};
    }
    if (gain_matrix->empty()) {
        logs.log(LogLevel::warning, "saf-vbap", "no renderable tracks found (all muted?), writing silence");
    }

    const auto num_in_ch = plan.scene.info.num_channels;
    const auto invalid_channel =
        std::ranges::find_if(*gain_matrix, [num_in_ch](const auto& cg) { return cg.input_channel >= num_in_ch; });
    if (invalid_channel != gain_matrix->end()) {
        return make_error(ErrorCode::render_failed,
                          fmt::format("track channel index {} is outside input channel count {}",
                                      invalid_channel->input_channel,
                                      num_in_ch),
                          "input=" + plan.input_path);
    }

    auto prepared = std::make_shared<VbapPrepared>();
    prepared->layout = std::move(*layout);
    auto compiled =
        render_common::prepare_speaker_mix(std::move(*gain_matrix), num_in_ch, prepared->layout.speakers.size());
    if (!compiled) {
        return tl::unexpected{compiled.error()};
    }
    prepared->gain_matrix = std::move(*compiled);
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

Result<RenderMetrics> VbapRenderer::render_window(const IPreparedRender& prep,
                                                  const RenderPlan& plan,
                                                  ProgressSink& progress,
                                                  LogSink& logs) {
    const auto* prepared = dynamic_cast<const VbapPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error, "saf-vbap: incompatible prepared state");
    }
    return render_common::render_speaker_pcm(plan,
                                             prepared->gain_matrix,
                                             static_cast<uint16_t>(prepared->layout.speakers.size()),
                                             "saf-vbap",
                                             progress,
                                             logs);
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
