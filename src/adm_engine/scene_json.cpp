#include "scene_json.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mradm::engine {

namespace {

using nlohmann::json;

constexpr uint64_t k_no_end = std::numeric_limits<uint64_t>::max();

// Append an optional scalar/string under `key` only when it has a value.
template <typename T> void add_opt(json& obj, const char* key, const std::optional<T>& value) {
    if (value.has_value()) {
        obj[key] = *value;
    }
}

// Append an end_sample only when it is not the "to end of file" sentinel.
void add_end_sample(json& obj, uint64_t end_sample) {
    if (end_sample != k_no_end) {
        obj["end_sample"] = end_sample;
    }
}

json loudness_to_json(const SceneLoudnessMetadata& lm) {
    json j = json::object();
    add_opt(j, "integrated_loudness", lm.integrated_loudness);
    add_opt(j, "max_true_peak", lm.max_true_peak);
    add_opt(j, "loudness_range", lm.loudness_range);
    add_opt(j, "max_momentary", lm.max_momentary);
    add_opt(j, "max_short_term", lm.max_short_term);
    add_opt(j, "dialogue_loudness", lm.dialogue_loudness);
    add_opt(j, "loudness_method", lm.loudness_method);
    return j;
}

// Block / offset positions: emit only the coordinates relevant to the
// coordinate system in use, plus the `cartesian` flag, matching inspect.
json position_to_json(const SceneBlockPosition& pos) {
    json j = json::object();
    j["cartesian"] = pos.cartesian;
    if (pos.cartesian) {
        j["x"] = pos.x;
        j["y"] = pos.y;
        j["z"] = pos.z;
    } else {
        j["azimuth"] = pos.azimuth;
        j["elevation"] = pos.elevation;
        j["distance"] = pos.distance;
    }
    return j;
}

json position_offset_to_json(const ScenePositionOffset& off) {
    json j = json::object();
    j["cartesian"] = off.cartesian;
    if (off.cartesian) {
        j["x"] = off.x;
        j["y"] = off.y;
        j["z"] = off.z;
    } else {
        j["azimuth"] = off.azimuth;
        j["elevation"] = off.elevation;
        j["distance"] = off.distance;
    }
    return j;
}

json object_block_to_json(const SceneObjectBlock& blk) {
    json j = json::object();
    j["position"] = position_to_json(blk.position);
    j["gain"] = blk.gain;
    j["diffuse"] = blk.diffuse;
    j["width"] = blk.width;
    j["height"] = blk.height;
    j["depth"] = blk.depth;
    j["start_sample"] = blk.start_sample;
    add_end_sample(j, blk.end_sample);
    j["jump_position"] = blk.jump_position;
    add_opt(j, "interp_length_samples", blk.interp_length_samples);
    j["channel_lock"] = blk.channel_lock;
    add_opt(j, "channel_lock_max_distance", blk.channel_lock_max_distance);
    j["divergence"] = blk.divergence;
    j["divergence_azimuth_range"] = blk.divergence_azimuth_range;
    j["divergence_position_range"] = blk.divergence_position_range;
    j["screen_ref"] = blk.screen_ref;
    return j;
}

json ds_block_to_json(const SceneDirectSpeakersBlock& blk) {
    json j = json::object();
    j["speaker_labels"] = blk.speaker_labels;
    j["pack_format_id"] = blk.pack_format_id;
    j["has_position"] = blk.has_position;
    if (blk.has_position) {
        j["azimuth"] = blk.azimuth;
        j["elevation"] = blk.elevation;
        j["distance"] = blk.distance;
    }
    add_opt(j, "azimuth_min", blk.azimuth_min);
    add_opt(j, "azimuth_max", blk.azimuth_max);
    add_opt(j, "elevation_min", blk.elevation_min);
    add_opt(j, "elevation_max", blk.elevation_max);
    add_opt(j, "distance_min", blk.distance_min);
    add_opt(j, "distance_max", blk.distance_max);
    j["gain"] = blk.gain;
    add_opt(j, "low_pass_hz", blk.low_pass_hz);
    j["start_sample"] = blk.start_sample;
    add_end_sample(j, blk.end_sample);
    return j;
}

json track_to_json(const SceneTrackRef& track) {
    json j = json::object();
    j["track_uid"] = track.track_uid;
    add_opt(j, "channel_index", track.channel_index);
    json object_blocks = json::array();
    std::ranges::transform(track.blocks, std::back_inserter(object_blocks), object_block_to_json);
    j["object_blocks"] = std::move(object_blocks);
    json ds_blocks = json::array();
    std::ranges::transform(track.ds_blocks, std::back_inserter(ds_blocks), ds_block_to_json);
    j["ds_blocks"] = std::move(ds_blocks);
    return j;
}

json object_to_json(const SceneObject& obj) {
    json j = json::object();
    j["id"] = obj.id;
    j["name"] = obj.name;
    j["gain"] = obj.gain;
    j["mute"] = obj.mute;
    add_end_sample(j, obj.end_sample);
    j["labels"] = obj.labels;
    add_opt(j, "importance", obj.importance);
    add_opt(j, "dialogue_id", obj.dialogue_id);
    if (obj.position_offset.has_value()) {
        j["position_offset"] = position_offset_to_json(*obj.position_offset);
    }
    json tracks = json::array();
    std::ranges::transform(obj.tracks, std::back_inserter(tracks), track_to_json);
    j["tracks"] = std::move(tracks);
    return j;
}

json programme_to_json(const SceneProgramme& p) {
    json j = json::object();
    j["id"] = p.id;
    j["name"] = p.name;
    add_opt(j, "language", p.language);
    j["labels"] = p.labels;
    j["start_sample"] = p.start_sample;
    add_opt(j, "end_sample", p.end_sample);
    j["has_reference_screen"] = p.has_reference_screen;
    if (p.loudness.has_value()) {
        j["loudness"] = loudness_to_json(*p.loudness);
    }
    j["content_ids"] = p.content_ids;
    return j;
}

json content_to_json(const SceneContent& c) {
    json j = json::object();
    j["id"] = c.id;
    j["name"] = c.name;
    add_opt(j, "language", c.language);
    j["labels"] = c.labels;
    add_opt(j, "dialogue_kind", c.dialogue_kind);
    add_opt(j, "content_kind", c.content_kind);
    if (c.loudness.has_value()) {
        j["loudness"] = loudness_to_json(*c.loudness);
    }
    j["object_ids"] = c.object_ids;
    return j;
}

json hoa_channel_to_json(const SceneHOAChannel& ch) {
    json j = json::object();
    add_opt(j, "channel_index", ch.channel_index);
    j["track_uid"] = ch.track_uid;
    j["order"] = ch.order;
    j["degree"] = ch.degree;
    return j;
}

json hoa_tracks_to_json(const SceneHOATracks& h) {
    json j = json::object();
    j["object_id"] = h.object_id;
    j["pack_format_id"] = h.pack_format_id;
    j["normalization"] = h.normalization;
    j["nfc_ref_dist"] = h.nfc_ref_dist;
    j["screen_ref"] = h.screen_ref;
    j["gain"] = h.gain;
    j["mute"] = h.mute;
    j["start_sample"] = h.start_sample;
    add_end_sample(j, h.end_sample);
    json channels = json::array();
    std::ranges::transform(h.channels, std::back_inserter(channels), hoa_channel_to_json);
    j["channels"] = std::move(channels);
    return j;
}

} // namespace

std::string scene_to_json(const AdmScene& scene) {
    json root = json::object();

    // Stable schema identity so bindings can reliably detect the layout version.
    // Bump on any breaking change to the field set; additive fields keep v1.
    root["schema"] = "mradm.scene-inspect";
    root["schema_version"] = 1;

    json file = json::object();
    file["path"] = scene.info.file_path;
    file["sample_rate"] = scene.info.sample_rate;
    file["num_channels"] = scene.info.num_channels;
    file["num_frames"] = scene.info.num_frames;
    if (scene.info.sample_rate > 0 && scene.info.num_frames > 0) {
        file["duration_seconds"] = static_cast<double>(scene.info.num_frames) / scene.info.sample_rate;
    }
    root["file"] = std::move(file);

    json programmes = json::array();
    std::ranges::transform(scene.programmes, std::back_inserter(programmes), programme_to_json);
    root["programmes"] = std::move(programmes);

    json contents = json::array();
    std::ranges::transform(scene.contents, std::back_inserter(contents), content_to_json);
    root["contents"] = std::move(contents);

    json objects = json::array();
    std::ranges::transform(scene.objects, std::back_inserter(objects), object_to_json);
    root["objects"] = std::move(objects);

    json hoa_tracks = json::array();
    std::ranges::transform(scene.hoa_tracks, std::back_inserter(hoa_tracks), hoa_tracks_to_json);
    root["hoa_tracks"] = std::move(hoa_tracks);

    root["import_warnings"] = scene.import_warnings;

    return root.dump(2);
}

} // namespace mradm::engine
