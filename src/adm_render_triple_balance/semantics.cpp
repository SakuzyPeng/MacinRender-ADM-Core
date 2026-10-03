#include "semantics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "adm/render.h"

#include "bed.h"
#include "layout_222.h"

namespace mradm::triple_balance {
namespace {
using Json = nlohmann::json;

Json decision(std::string_view action, const Json& value, const Json& effective) {
    return {{"action", action}, {"source", value}, {"renderer", effective}};
}
} // namespace

float user_output_gain(const SceneObject& object) {
    return object.user_level.mute.value_or(false) ? 0.0F : object.user_level.gain_multiplier;
}

// NOLINTNEXTLINE(readability-function-size): compile render data and its audit report in the same pass.
Result<AdmScene> prepare_semantics(const RenderPlan& plan, std::string& report) {
    AdmScene scene = plan.scene;
    Json document = {{"profile", "triple-balance-objects-v1"},
                     {"profile_version", 1},
                     {"layout", plan.output_layout},
                     {"sample_rate", scene.info.sample_rate},
                     {"status", "prepared"},
                     {"time_basis", "file_absolute_samples"},
                     {"control_frames", 512},
                     {"event_stage", "targets before existing room conversion, smoothing and quantization"},
                     {"objects", Json::array()},
                     {"bed_profile", "triple-balance-7.1.2-bed-v1"},
                     {"beds", Json::array()}};
    if (is_room_222(plan.output_layout)) {
        document["profile"] = "room-222-extension-v1";
        document["bed_profile"] = "source-7.1.2-to-room-222-v1";
        document["source_semantics_profile"] = "triple-balance-objects-v1";
        document["spatial_reference"] = nullptr;
        document["spatial_model"] = "self-defined three-layer equal-power room";
        document["size_model"] = "exact mean point power over a clipped Cartesian cube of half-width=size";
        document["event_stage"] = "targets before float smoothing; continuous unquantized room geometry";
        document["lower_layer_rule"] = "project Y onto the three-speaker front row";
        document["lfe_routing"] = plan.lfe_routing_mode == LfeRoutingMode::direct ? "direct" : "split-power";
        document["spatial_nodes"] = Json::array();
        for (const auto& node : room_222_nodes()) {
            document["spatial_nodes"].push_back({{"channel", node.channel},
                                                 {"label", node.label},
                                                 {"xyz", Json::array({node.x, node.y, node.z})},
                                                 {"filter", node.filter},
                                                 {"filter_sign", node.sign}});
        }
    }
    std::set<uint16_t> input_channels;
    std::size_t bed_count = 0;
    const auto reject = [&](const std::string& object,
                            const std::string& field,
                            const std::string& reason,
                            const std::string& context = std::string{}) -> Result<AdmScene> {
        document["status"] = "unsupported";
        document["rejection"] = {{"object_id", object}, {"field", field}, {"reason", reason}};
        if (!context.empty()) {
            document["rejection"]["context"] = context;
        }
        report = document.dump();
        return make_error(
            ErrorCode::unsupported, reason, context.empty() ? "object=" + object + "; field=" + field : context);
    };
    for (auto& object : scene.objects) {
        if (object.adm_source && (object.adm_source->has_parent || !object.adm_source->child_objects.empty())) {
            return reject(object.id, "audioObjectIDRef", "triple-balance has not verified nested object semantics");
        }
        for (const auto& track : object.tracks) {
            if (track.blocks.empty() && track.ds_blocks.empty()) {
                return reject(
                    object.id, "audioBlockFormat", "triple-balance requires renderable metadata for each track");
            }
            if (track.channel_index && !input_channels.insert(*track.channel_index).second) {
                return reject(object.id, "audioTrackUIDRef", "triple-balance requires independently bound PCM tracks");
            }
        }
        const bool has_objects =
            std::ranges::any_of(object.tracks, [](const auto& track) { return !track.blocks.empty(); });
        const bool has_bed =
            std::ranges::any_of(object.tracks, [](const auto& track) { return !track.ds_blocks.empty(); });
        if (has_objects && has_bed) {
            return reject(object.id, "audioPackFormat", "triple-balance requires separate bed and Objects owners");
        }
        if (!has_objects) {
            if (++bed_count > 1) {
                return reject(object.id, "audioPackFormat", "triple-balance has not verified multiple beds");
            }
            if (plan.direct_speakers_routing_mode == DirectSpeakersRoutingMode::position ||
                plan.direct_speakers_routing_mode == DirectSpeakersRoutingMode::matrix || plan.direct_speakers_matrix) {
                return reject(object.id, "direct_speakers_routing", "triple-balance bed uses fixed reference routing");
            }
            auto valid = validate_bed(object, scene.info);
            if (!valid) {
                return reject(object.id, "bed", valid.error().message, valid.error().context);
            }
        }
        const float native_gain = object.adm_source ? object.adm_source->gain.linear : object.gain;
        const bool native_mute = object.adm_source ? object.adm_source->mute : object.mute;
        const uint64_t native_start = object.adm_source ? object.adm_source->start_samples : 0;
        if (!std::isfinite(native_gain) || native_gain < 0 || !std::isfinite(object.user_level.gain_multiplier) ||
            object.user_level.gain_multiplier < 0) {
            return reject(object.id, "gain", "triple-balance requires finite nonnegative gain");
        }
        if (scene.info.sample_rate != 48000 &&
            (native_gain != 1 || native_mute || native_start != 0 || object.end_sample < scene.info.num_frames ||
             object.user_level.gain_multiplier != 1 || object.user_level.mute.has_value())) {
            return reject(object.id, "sample_rate", "triple-balance extended object semantics require 48 kHz");
        }
        Json row = {{"id", object.id},
                    {"native_gain", decision("ignored", native_gain, 1)},
                    {"native_mute", decision("ignored", native_mute, false)},
                    {"object_start", decision("ignored", native_start, 0)},
                    {"object_end",
                     decision("ignored",
                              object.end_sample == std::numeric_limits<uint64_t>::max() ? Json(nullptr)
                                                                                        : Json(object.end_sample),
                              scene.info.num_frames)},
                    {"user_gain_multiplier", object.user_level.gain_multiplier},
                    {"user_mute", object.user_level.mute ? Json(*object.user_level.mute) : Json(nullptr)},
                    {"output_gain", user_output_gain(object)},
                    {"output_gain_stage", has_bed ? "after_fixed_bed_route" : "after_object_dry_and_size_mix"},
                    {"lifecycle", "PCM active throughout file; metadata durations do not gate or reset DSP"},
                    {"tracks", Json::array()}};
        object.gain = 1;
        object.mute = false;
        object.end_sample = scene.info.num_frames;
        for (auto& track : object.tracks) {
            if (track.blocks.empty()) {
                Json events = Json::array();
                for (const auto& block : track.ds_blocks) {
                    const auto native = block.adm_source ? block.adm_source->gain.linear : block.gain;
                    events.push_back(
                        {{"source_absolute_start_sample", block.start_sample},
                         {"source_rtime_sample",
                          block.adm_source ? Json(block.adm_source->rtime_samples) : Json(nullptr)},
                         {"source_end_sample",
                          block.end_sample == std::numeric_limits<uint64_t>::max() ? Json(nullptr)
                                                                                   : Json(block.end_sample)},
                         {"gain", decision("ignored", native, 1)},
                         {"time", "ignored; fixed route active throughout file"}});
                }
                auto& block = track.ds_blocks.front();
                auto gains = bed_gains(block, plan.output_layout, plan.lfe_routing_mode);
                if (!gains) {
                    return reject(object.id, "output_layout", gains.error().message);
                }
                row["tracks"].push_back(
                    {{"track_uid", track.track_uid},
                     {"pcm_channel", *track.channel_index},
                     {"speaker_labels", block.speaker_labels},
                     {"position", "ignored; verified bed channel identity selects routing"},
                     {"gains_internal_layout_order", *gains},
                     {"user_gain_multiplier", block.user_level.gain_multiplier},
                     {"user_mute", block.user_level.mute ? Json(*block.user_level.mute) : Json(nullptr)},
                     {"output_gain", user_output_gain(object) * bed_user_gain(block)},
                     {"events", events}});
                block.start_sample = 0;
                block.end_sample = std::numeric_limits<uint64_t>::max();
                block.gain = 1;
                track.ds_blocks.resize(1);
                continue;
            }
            Json events = Json::array();
            uint64_t previous = 0;
            bool first = true;
            for (auto& block : track.blocks) {
                const uint64_t relative = block.adm_source ? block.adm_source->rtime_samples : block.start_sample;
                if (!first && (relative <= previous || relative / 512 == previous / 512)) {
                    return reject(object.id, "rtime", "triple-balance requires one ordered update per 512-frame block");
                }
                if (relative >= scene.info.num_frames) {
                    return reject(
                        object.id, "rtime", "triple-balance metadata update falls outside the audio timeline");
                }
                if (!std::isfinite(block.gain) || block.gain < 0) {
                    return reject(object.id, "block.gain", "triple-balance requires finite nonnegative block gain");
                }
                events.push_back(
                    {{"source_absolute_start_sample", block.start_sample},
                     {"source_rtime_sample", relative},
                     {"renderer_event_sample", relative / 512 * 512},
                     {"control_start_sample", relative / 512 * 512},
                     {"target_cartesian", block.position.cartesian},
                     {"target_xyz", Json::array({block.position.x, block.position.y, block.position.z})},
                     {"extent",
                      decision(plan.speaker_spread_mode == SpeakerSpreadMode::none ? "ignored_by_user" : "used",
                               Json::array({block.width, block.height, block.depth}),
                               plan.speaker_spread_mode == SpeakerSpreadMode::none
                                   ? Json::array({0, 0, 0})
                                   : Json::array({block.width, block.height, block.depth}))},
                     {"gain", decision("ignored", block.gain, 1)},
                     {"end",
                      decision("ignored",
                               block.end_sample == std::numeric_limits<uint64_t>::max() ? Json(nullptr)
                                                                                        : Json(block.end_sample),
                               scene.info.num_frames)}});
                if (is_room_222(plan.output_layout)) {
                    const float extent = plan.speaker_spread_mode == SpeakerSpreadMode::none ? 0 : block.width;
                    auto target = room_222_gains(block.position, extent);
                    if (!target) {
                        return reject(object.id,
                                      "position/size",
                                      target.error().message,
                                      "object=" + object.id + "; track=" + track.track_uid +
                                          "; sample=" + std::to_string(block.start_sample));
                    }
                    const auto mix = room_222_mix(*target, extent);
                    events.back()["target_geometry_raw_gains"] = *target;
                    events.back()["target_mix_direct"] = mix.direct;
                    events.back()["target_mix_spread_gains"] = mix.spread;
                }
                block.start_sample = relative;
                block.end_sample = std::numeric_limits<uint64_t>::max();
                block.gain = 1;
                previous = relative;
                first = false;
            }
            const auto initial_time = track.blocks.front().start_sample;
            if (initial_time > 0) {
                if (scene.info.sample_rate != 48000) {
                    return reject(
                        object.id, "first.rtime", "triple-balance has not verified this initial metadata block");
                }
                if (initial_time < 512) {
                    track.blocks.front().start_sample = 0;
                } else {
                    SceneObjectBlock initial;
                    initial.position.cartesian = true;
                    initial.jump_position = true;
                    track.blocks.insert(track.blocks.begin(), initial);
                }
            }
            row["tracks"].push_back(
                {{"track_uid", track.track_uid},
                 {"pcm_channel", track.channel_index ? Json(*track.channel_index) : Json(nullptr)},
                 {"initial_default_position", initial_time >= 512 ? Json::array({0, 0, 0}) : Json(nullptr)},
                 {"initial_default_size", initial_time >= 512 ? Json(0) : Json(nullptr)},
                 {"events", events}});
        }
        document[has_bed ? "beds" : "objects"].push_back(std::move(row));
    }
    report = document.dump();
    return scene;
}

void publish_semantics(const RenderPlan& plan, const std::string& report, const Error* error) {
    if (!plan.renderer_semantics_sink) {
        return;
    }
    auto document = report.empty() ? Json::object() : Json::parse(report);
    if (error != nullptr) {
        document["status"] = error->code == ErrorCode::unsupported ? "unsupported" : "rejected";
        document["error"] = {{"message", error->message}, {"context", error->context}};
    }
    plan.renderer_semantics_sink(document.dump());
}
} // namespace mradm::triple_balance
