#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "adm/render_triple_balance.h"

#include "bed.h"
#include "layout_222.h"
#include "panner.h"
#include "render_common.h"
#include "render_state.h"
#include "semantics.h"
#include "size_processor.h"
#include "speaker_layouts.h"
#include "speaker_pcm.h"

namespace mradm {
namespace {
using render_common::BlockGains;
using render_common::ChannelGainInfo;

// Frozen 48 kHz isotropic-size boundary and independent sets pass both layouts.
constexpr bool k_triple_balance_size_verified = true;
// Self-defined 22.2 geometry and state validation.
constexpr bool k_room_222_size_verified = true;

bool uses_room_size(const SceneTrackRef& track, SpeakerSpreadMode spread_mode) {
    return spread_mode != SpeakerSpreadMode::none && std::ranges::any_of(track.blocks, [](const auto& block) {
               return block.width != 0 || block.height != 0 || block.depth != 0;
           });
}

std::vector<triple_balance::SizeEvent> size_events(const SceneTrackRef& track) {
    std::vector<triple_balance::SizeEvent> events;
    events.reserve(track.blocks.size());
    std::ranges::transform(track.blocks, std::back_inserter(events), [](const auto& block) {
        return triple_balance::SizeEvent{
            block.start_sample,
            {(block.position.x + 1) * 0.5F, (1 - block.position.y) * 0.5F, block.position.z},
            block.width};
    });
    return events;
}

[[nodiscard]] Result<std::vector<BlockGains>> triple_balance_motion_blocks(const SceneTrackRef& track,
                                                                           const SceneObject& object,
                                                                           std::string_view layout_id,
                                                                           uint64_t file_frames) {
    if (track.blocks.empty()) {
        return std::vector<BlockGains>{};
    }
    constexpr uint64_t k_control_frames = 512;
    constexpr float k_position_tau_frames = 1200.0F;
    const float alpha = 1.0F - std::exp(-static_cast<float>(k_control_frames) / k_position_tau_frames);
    const auto& first = track.blocks.front();
    if (first.start_sample != 0 || object.end_sample < file_frames) {
        return make_error(ErrorCode::unsupported,
                          "triple-balance requires an object present from frame 0 through the file end");
    }
    // The reference smooths float32 internal room coordinates. Double state
    // eventually rounds across half-code boundaries (notably Z=0.5), while the
    // real float state can remain one ULP below the target indefinitely.
    std::array<float, 3> state{(first.position.x + 1) * 0.5F, (1 - first.position.y) * 0.5F, first.position.z};
    std::array<float, 3> target = state;
    // The compatibility profile ignores audioBlockFormat/gain for
    // Objects. Static 0/-6/-12 dB probes and a changing-gain probe both retain
    // unit gain in the exported speaker PCM. This is specific to this
    // compatibility mode; the other backends retain their own ADM gain semantics.
    auto initial = triple_balance::point_gains(first.position, 1.0F, layout_id);
    if (!initial) {
        return tl::unexpected{initial.error()};
    }
    std::vector<BlockGains> result;
    result.push_back({std::move(*initial), 0, std::numeric_limits<uint64_t>::max(), true, true, std::nullopt});
    if (track.blocks.size() == 1) {
        return result;
    }
    std::size_t next_event = 1;
    for (uint64_t control_start = k_control_frames; control_start < file_frames; control_start += k_control_frames) {
        if (next_event < track.blocks.size() &&
            track.blocks[next_event].start_sample < control_start + k_control_frames) {
            const auto& event = track.blocks[next_event];
            if (event.start_sample < control_start ||
                (next_event + 1 < track.blocks.size() &&
                 track.blocks[next_event + 1].start_sample < control_start + k_control_frames)) {
                return make_error(ErrorCode::unsupported,
                                  "triple-balance supports at most one metadata update per 512-frame control block");
            }
            target = {(event.position.x + 1) * 0.5F, (1 - event.position.y) * 0.5F, event.position.z};
            ++next_event;
        }
        state = {state[0] + (alpha * (target[0] - state[0])),
                 state[1] + (alpha * (target[1] - state[1])),
                 state[2] + (alpha * (target[2] - state[2]))};
        SceneBlockPosition position;
        position.cartesian = true;
        position.x = (state[0] * 2) - 1;
        position.y = 1 - (state[1] * 2);
        position.z = state[2];
        auto gains = triple_balance::point_gains(position, 1.0F, layout_id);
        if (!gains) {
            return tl::unexpected{gains.error()};
        }
        if (*gains != result.back().gains) {
            result.push_back({std::move(*gains),
                              control_start,
                              std::numeric_limits<uint64_t>::max(),
                              false,
                              true,
                              k_control_frames});
        }
    }
    if (next_event != track.blocks.size()) {
        return make_error(ErrorCode::unsupported, "triple-balance metadata update falls outside the audio timeline");
    }
    return result;
}

[[nodiscard]] Result<void> validate_triple_balance_input(const RenderPlan& plan) {
    const bool extended = triple_balance::is_room_222(plan.output_layout);
    if (plan.output_layout != "4+7+0" && plan.output_layout != "9.1.6" && !extended) {
        return make_error(ErrorCode::unsupported, "room renderer supports 7.1.4/9.1.6 and experimental 22.2");
    }
    if (extended && plan.scene.info.sample_rate != 48000) {
        return make_error(ErrorCode::unsupported, "experimental room-222 requires 48 kHz");
    }
    if (plan.scene.info.source_kind != SceneSourceKind::adm || plan.speaker_geometry != SpeakerGeometry::standard ||
        plan.speaker_spread_mode == SpeakerSpreadMode::mdap || plan.object_smoothing_frames != 0U ||
        !plan.scene.hoa_tracks.empty()) {
        return make_error(ErrorCode::unsupported,
                          "triple-balance requires ADM input, fixed room geometry, no MDAP/extra smoothing or HOA");
    }
    if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic || plan.direct_speakers_matrix) {
        return make_error(ErrorCode::unsupported,
                          "triple-balance uses fixed bed routing; explicit routing is unsupported");
    }
    for (const auto& object : plan.scene.objects) {
        if (object.mute) {
            continue;
        }
        if (object.position_offset) {
            return make_error(ErrorCode::unsupported,
                              "triple-balance does not yet support positionOffset",
                              "object=" + object.id + "; field=positionOffset");
        }
        if (object.gain != 1.0F) {
            return make_error(ErrorCode::unsupported,
                              "triple-balance has not verified native DirectSpeakers audioObject gain",
                              "object=" + object.id + "; field=gain");
        }
        for (const auto& track : object.tracks) {
            if (!track.channel_index) {
                return make_error(ErrorCode::unsupported,
                                  "triple-balance requires a PCM channel for each active track");
            }
            for (const auto& block : track.blocks) {
                const bool has_extent = block.width != 0.0F || block.height != 0.0F || block.depth != 0.0F;
                if (extended) {
                    auto valid_position = triple_balance::room_222_gains(block.position);
                    if (!valid_position) {
                        return make_error(valid_position.error().code,
                                          valid_position.error().message,
                                          fmt::format("object={}; track={}; sample={}; field=position",
                                                      object.id,
                                                      track.track_uid,
                                                      block.start_sample));
                    }
                }
                if (extended && has_extent && plan.speaker_spread_mode != SpeakerSpreadMode::none &&
                    !k_room_222_size_verified) {
                    return make_error(ErrorCode::unsupported,
                                      "room-222 size awaits independent geometry/state checks",
                                      "object=" + object.id + "; field=size");
                }
                const bool native_extent = k_triple_balance_size_verified && has_extent &&
                                           plan.scene.info.sample_rate == 48000 && std::isfinite(block.width) &&
                                           block.width >= 0 && block.width <= 1 && block.width == block.height &&
                                           block.width == block.depth && (block.diffuse == 0 || block.diffuse == 1);
                if (!block.position.cartesian ||
                    ((has_extent || block.diffuse != 0.0F) && plan.speaker_spread_mode != SpeakerSpreadMode::none &&
                     !native_extent) ||
                    block.divergence != 0.0F || block.channel_lock || block.screen_ref || block.head_locked) {
                    return make_error(ErrorCode::unsupported,
                                      "triple-balance requires verified Cartesian point or 48 kHz equal-size Objects; "
                                      "spread=none explicitly ignores extent/diffuse",
                                      fmt::format("object={}; track={}; sample={}; field=position/extent/modifiers",
                                                  object.id,
                                                  track.track_uid,
                                                  block.start_sample));
                }
            }
            if (uses_room_size(track, plan.speaker_spread_mode)) {
                if (object.end_sample < plan.scene.info.num_frames || track.blocks.empty() ||
                    track.blocks.back().start_sample >= plan.scene.info.num_frames) {
                    return make_error(ErrorCode::unsupported,
                                      "triple-balance size requires an object spanning the file");
                }
                auto checked = triple_balance::SizeObjectProcessor::create(
                    size_events(track), plan.output_layout, plan.scene.info.sample_rate);
                if (!checked) {
                    return tl::unexpected{checked.error()};
                }
            }
        }
    }
    return {};
}

using TripleBalancePrepared = triple_balance::Prepared;

class TripleBalanceRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override { return triple_balance_capabilities(); }
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& source, LogSink& logs) override;
    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& state, const RenderPlan& plan, LogSink& logs) override {
        (void) logs;
        const auto* prepared = dynamic_cast<const TripleBalancePrepared*>(&state);
        if (prepared == nullptr) {
            return make_error(ErrorCode::internal_error, "triple-balance: incompatible prepared state");
        }
        return triple_balance::open_stream(*prepared, plan);
    }
    [[nodiscard]] Result<RenderMetrics>
    render_window(const IPreparedRender& state, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

// NOLINTNEXTLINE(readability-function-size): validation, prepared metadata and semantics report share one transaction.
Result<std::shared_ptr<IPreparedRender>> TripleBalanceRenderer::prepare(const RenderPlan& source, LogSink& logs) {
    std::string report;
    auto scene = triple_balance::prepare_semantics(source, report);
    if (!scene) {
        triple_balance::publish_semantics(source, report, &scene.error());
        return tl::unexpected{scene.error()};
    }
    auto plan = source;
    plan.scene = std::move(*scene);
    const auto fail = [&](const Error& error) -> Result<std::shared_ptr<IPreparedRender>> {
        triple_balance::publish_semantics(plan, report, &error);
        return tl::unexpected{error};
    };
    const auto valid = validate_triple_balance_input(plan);
    if (!valid) {
        return fail(valid.error());
    }
    const auto* layout = render_layouts::find_speaker_layout(plan.output_layout);
    if (layout == nullptr) {
        return fail(Error{ErrorCode::unsupported, "unsupported Triple Balance output layout", plan.output_layout});
    }
    logs.log(LogLevel::info,
             "triple-balance",
             triple_balance::is_room_222(plan.output_layout)
                 ? "Triple Balance: experimental self-defined 22.2 extension"
                 : "Triple Balance: Cartesian Objects and verified 7.1.2 bed");
    auto lfe = render_common::resolve_lfe_routing(plan, logs, "triple-balance");
    if (!lfe) {
        return fail(lfe.error());
    }
    auto prepared = std::make_shared<TripleBalancePrepared>();
    prepared->output_channels = static_cast<uint16_t>(layout->speakers.size());
    prepared->sample_rate = plan.scene.info.sample_rate;
    prepared->spread_mode = plan.speaker_spread_mode;
    std::map<uint16_t, ChannelGainInfo> by_channel;
    for (const auto& object : plan.scene.objects) {
        if (object.mute) {
            continue;
        }
        for (const auto& track : object.tracks) {
            const auto channel = track.channel_index.value_or(plan.scene.info.num_channels);
            if (channel >= plan.scene.info.num_channels) {
                return fail(Error{ErrorCode::render_failed,
                                  fmt::format("track channel index {} is outside input channel count {}",
                                              channel,
                                              plan.scene.info.num_channels),
                                  "input=" + plan.input_path});
            }
            auto& gains = by_channel[channel];
            gains.input_channel = channel;
            gains.object_id = object.id;
            gains.output_gain = triple_balance::user_output_gain(object);
            if (!track.ds_blocks.empty()) {
                auto bed = triple_balance::bed_gains(track.ds_blocks.front(), plan.output_layout, lfe->mode);
                if (!bed) {
                    return fail(bed.error());
                }
                gains.speaker_label_key =
                    render_common::canonicalise_speaker_label(track.ds_blocks.front().speaker_labels.front());
                gains.output_gain *= triple_balance::bed_user_gain(track.ds_blocks.front());
                gains.blocks.push_back({std::move(*bed), 0, plan.scene.info.num_frames, true, false, std::nullopt});
            } else if (uses_room_size(track, plan.speaker_spread_mode)) {
                const bool diffuse =
                    std::ranges::any_of(track.blocks, [](const auto& block) { return block.diffuse != 0; });
                float minimum_diffuse_size = 1.0F;
                for (const auto& block : track.blocks) {
                    if (block.diffuse != 0) {
                        minimum_diffuse_size = std::min(minimum_diffuse_size, block.width);
                    }
                }
                prepared->size_tracks.push_back({channel,
                                                 size_events(track),
                                                 gains.output_gain,
                                                 object.id,
                                                 diffuse,
                                                 minimum_diffuse_size,
                                                 track.blocks.front().position});
            } else {
                auto motion =
                    triple_balance_motion_blocks(track, object, plan.output_layout, plan.scene.info.num_frames);
                if (!motion) {
                    return fail(motion.error());
                }
                gains.blocks = std::move(*motion);
            }
        }
    }
    std::vector<ChannelGainInfo> matrix;
    for (auto& [channel, gains] : by_channel) {
        std::ranges::sort(gains.blocks, {}, &BlockGains::start_sample);
        matrix.push_back(std::move(gains));
    }
    auto compiled =
        render_common::prepare_speaker_mix(std::move(matrix), plan.scene.info.num_channels, prepared->output_channels);
    if (!compiled) {
        return fail(compiled.error());
    }
    prepared->gain_matrix = std::move(*compiled);
    if (prepared->gain_matrix.empty()) {
        logs.log(LogLevel::warning, "triple-balance", "no renderable tracks found, writing silence");
    }
    triple_balance::publish_semantics(plan, report);
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

Result<RenderMetrics> TripleBalanceRenderer::render_window(const IPreparedRender& state,
                                                           const RenderPlan& plan,
                                                           ProgressSink& progress,
                                                           LogSink& logs) {
    const auto* prepared = dynamic_cast<const TripleBalancePrepared*>(&state);
    if (prepared == nullptr) {
        return make_error(ErrorCode::internal_error, "triple-balance: incompatible prepared state");
    }
    if (prepared->size_tracks.empty()) {
        return render_common::render_speaker_pcm(
            plan, prepared->gain_matrix, prepared->output_channels, "triple-balance", progress, logs);
    }
    auto mixer = triple_balance::SizeMixer::create(*prepared, plan);
    if (!mixer) {
        return tl::unexpected{mixer.error()};
    }
    uint64_t position = 0;
    const render_common::SpeakerBlockProcessor process =
        [&](std::span<const float> source, std::span<float> mixed, bool final) -> Result<void> {
        auto status = mixer->process(source, mixed, position, final);
        position += source.size() / plan.scene.info.num_channels;
        return status;
    };
    return render_common::render_speaker_pcm(
        plan, prepared->gain_matrix, prepared->output_channels, "triple-balance", progress, logs, process);
}

} // namespace

CapabilityReport triple_balance_capabilities() {
    CapabilityReport result;
    result.backend_name = "triple-balance";
    result.backend_version = "1";
    result.supports_objects = true;
    result.supports_direct_speakers = true;
    result.supports_render_window = true;
    // Fixed room layouts; no custom SAF layouts or listener-oriented geometry.
    result.supported_layouts = {
        {"4+7+0", "7.1.4", 12, true, 1, true, false},
        {"9.1.6", "9.1.6", 16, true, 1, true, false},
        {"9+10+3", "22.2 (experimental)", 24, true, 2, true, false},
    };
    return result;
}

std::unique_ptr<IRenderer> create_triple_balance_renderer() {
    return std::make_unique<TripleBalanceRenderer>();
}

} // namespace mradm
