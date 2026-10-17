#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "adm/semantic_policy.h"

namespace {

bool check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        return false;
    }
    return true;
}

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~FileGuard() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;

  private:
    std::filesystem::path path_;
};

std::filesystem::path write_temp_json(const std::string& name, const std::string& text) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    out << text;
    return path;
}

// The C++ path parses in-memory text; files are read here (the offline policy is Rust since 4.4).
mradm::Result<mradm::SemanticPolicy> load_policy(const std::filesystem::path& path) {
    std::ifstream in(path);
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return mradm::parse_semantic_policy(text, path.string());
}

mradm::AdmScene make_scene() {
    mradm::AdmScene scene;
    scene.info.sample_rate = 48000U;

    mradm::SceneObject kick;
    kick.id = "AO_1001";
    kick.name = "Kick Stem";
    mradm::SceneTrackRef kick_track;
    kick_track.track_uid = "ATU_00000001";
    mradm::SceneObjectBlock kick_block;
    kick_block.diffuse = 1.0F;
    kick_block.width = 0.4F;
    kick_block.height = 0.2F;
    kick_block.depth = 0.1F;
    kick_block.divergence = 0.5F;
    kick_block.divergence_azimuth_range = 90.0F;
    kick_block.divergence_position_range = 0.25F;
    kick_block.channel_lock = true;
    kick_block.channel_lock_max_distance = 0.5F;
    kick_block.jump_position = true;
    kick_block.interp_length_samples = 9600U;
    kick_track.blocks.push_back(kick_block);
    kick.tracks.push_back(kick_track);

    mradm::SceneObject cymbal;
    cymbal.id = "AO_1002";
    cymbal.name = "Cymbal Stem";
    mradm::SceneTrackRef cymbal_track;
    cymbal_track.track_uid = "ATU_00000002";
    mradm::SceneObjectBlock cymbal_block;
    cymbal_block.diffuse = 0.5F;
    cymbal_block.width = 0.5F;
    cymbal_block.height = 0.5F;
    cymbal_block.depth = 0.5F;
    cymbal_track.blocks.push_back(cymbal_block);
    cymbal.tracks.push_back(cymbal_track);

    scene.objects.push_back(kick);
    scene.objects.push_back(cymbal);
    return scene;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 1.0e-5F;
}

bool verify_policy_parse_and_apply() {
    const auto path = write_temp_json("mr_semantic_policy_test.json",
                                      R"json({
  "schema": "mradm.semantic-policy.v1",
  "global": {
    "diffuse": { "scale": 0.5, "max": 0.3 },
    "extent": { "scale": 0.5, "height_scale": 0.25, "max": 0.2 },
    "divergence": { "scale": 0.5, "range_scale": 0.5, "max_range": 30.0 },
    "channel_lock": { "enabled": false },
    "interpolation": { "honor_jump_position": false, "max_ms": 50 }
  },
  "objects": [
    {
      "name_glob": "*kick*",
      "diffuse": { "enabled": false },
      "extent": { "enabled": false }
    }
  ]
})json");
    const FileGuard guard{path};

    auto policy = load_policy(path);
    if (!check(policy.has_value(), "policy parses")) {
        if (!policy) {
            std::cerr << policy.error().message << "\n";
        }
        return false;
    }

    auto scene = make_scene();
    std::vector<std::string> warnings;
    auto result = mradm::apply_semantic_policy(scene, *policy, scene.info.sample_rate, &warnings);
    if (!check(result.has_value(), "policy applies")) {
        return false;
    }
    bool ok = true;
    ok &= check(warnings.empty(), "all policy object rules matched");

    const auto& kick = scene.objects.at(0).tracks.at(0).blocks.at(0);
    ok &= check(near(kick.diffuse, 0.0F), "object rule disables kick diffuse");
    ok &= check(near(kick.width, 0.0F) && near(kick.height, 0.0F) && near(kick.depth, 0.0F),
                "object rule disables kick extent");
    ok &= check(near(kick.divergence, 0.25F), "global divergence scale applied");
    ok &= check(near(kick.divergence_azimuth_range, 30.0F), "global divergence range clamped");
    ok &= check(!kick.channel_lock && !kick.channel_lock_max_distance.has_value(), "channelLock disabled");
    ok &= check(!kick.jump_position, "jumpPosition ignored");
    ok &= check(kick.interp_length_samples == 2400U, "interpolationLength max_ms clamped");

    const auto& cymbal = scene.objects.at(1).tracks.at(0).blocks.at(0);
    ok &= check(near(cymbal.diffuse, 0.25F), "global diffuse scale/max applied");
    ok &= check(near(cymbal.width, 0.2F), "extent width clamped");
    ok &= check(near(cymbal.height, 0.0625F), "extent height scaled");
    ok &= check(near(cymbal.depth, 0.2F), "extent depth clamped");
    return ok;
}

bool verify_invalid_policy_rejected() {
    const auto path = write_temp_json("mr_semantic_policy_bad_test.json",
                                      R"json({
  "schema": "mradm.semantic-policy.v1",
  "global": { "diffuse": { "scle": 1.0 } }
})json");
    const FileGuard guard{path};
    const auto policy = load_policy(path);
    bool ok = true;
    ok &= check(!policy.has_value(), "unknown field rejected");
    if (!policy) {
        ok &= check(policy.error().code == mradm::ErrorCode::invalid_argument, "invalid policy error code");
    }
    // A number outside double range is an invalid policy, not an escaping exception.
    const auto overflow = mradm::parse_semantic_policy(
        R"json({"schema":"mradm.semantic-policy.v1","global":{"gain":{"gain_db":1e400}}})json", "<memory>");
    ok &= check(!overflow.has_value() && overflow.error().code == mradm::ErrorCode::invalid_argument,
                "number overflow rejected as invalid policy");
    return ok;
}

bool verify_policy_id_matching_is_case_insensitive() {
    const auto path = write_temp_json("mr_semantic_policy_id_case_test.json",
                                      R"json({
  "schema": "mradm.semantic-policy.v1",
  "objects": [
    {
      "id": "ao_1001",
      "diffuse": { "enabled": false }
    },
    {
      "track_uid": "atu_00000002",
      "extent": { "enabled": false }
    }
  ]
})json");
    const FileGuard guard{path};

    auto policy = load_policy(path);
    if (!check(policy.has_value(), "case-insensitive id policy parses")) {
        return false;
    }

    auto scene = make_scene();
    std::vector<std::string> warnings;
    auto result = mradm::apply_semantic_policy(scene, *policy, scene.info.sample_rate, &warnings);
    if (!check(result.has_value(), "case-insensitive id policy applies")) {
        return false;
    }

    bool ok = true;
    ok &= check(warnings.empty(), "case-insensitive id/track_uid rules matched");
    const auto& kick = scene.objects.at(0).tracks.at(0).blocks.at(0);
    ok &= check(near(kick.diffuse, 0.0F), "object id match ignores case");
    const auto& cymbal = scene.objects.at(1).tracks.at(0).blocks.at(0);
    ok &= check(near(cymbal.width, 0.0F) && near(cymbal.height, 0.0F) && near(cymbal.depth, 0.0F),
                "track_uid match ignores case");
    return ok;
}

mradm::AdmScene make_rich_scene() {
    mradm::AdmScene scene;
    scene.info.sample_rate = 48000U;

    mradm::SceneObject dialog;
    dialog.id = "AO_2001";
    dialog.name = "Dialogue";
    dialog.importance = 9;
    dialog.dialogue_id = 1U; // dialogue
    mradm::SceneTrackRef dtrack;
    dtrack.track_uid = "ATU_00000001";
    dtrack.blocks.push_back(mradm::SceneObjectBlock{});
    dialog.tracks.push_back(dtrack);

    mradm::SceneObject amb;
    amb.id = "AO_2002";
    amb.name = "Ambience";
    amb.importance = 2;
    amb.dialogue_id = 0U; // non-dialogue
    mradm::SceneTrackRef atrack;
    atrack.track_uid = "ATU_00000002";
    atrack.blocks.push_back(mradm::SceneObjectBlock{});
    amb.tracks.push_back(atrack);

    scene.objects.push_back(dialog);
    scene.objects.push_back(amb);

    mradm::SceneContent content;
    content.id = "ACO_1001";
    content.name = "Main Dialogue";
    content.object_ids = {"AO_2001"};
    scene.contents.push_back(content);

    mradm::SceneProgramme programme;
    programme.id = "APR_1001";
    programme.name = "English";
    programme.content_ids = {"ACO_1001"};
    scene.programmes.push_back(programme);
    return scene;
}

// Scene with a single DirectSpeakers bed object: one normal channel (M+000) and
// one LFE channel (low_pass_hz set), each on its own track.
mradm::AdmScene make_ds_scene() {
    mradm::AdmScene scene;
    scene.info.sample_rate = 48000U;

    mradm::SceneObject bed;
    bed.id = "AO_4001";
    bed.name = "Bed";

    mradm::SceneTrackRef center_track;
    center_track.track_uid = "ATU_00000001";
    mradm::SceneDirectSpeakersBlock center;
    center.speaker_labels = {"M+000"};
    center.gain = 1.0F;
    center.has_position = true;
    center.azimuth = 0.0F;
    center.elevation = 0.0F;
    center.distance = 1.0F;
    center_track.ds_blocks.push_back(center);
    bed.tracks.push_back(center_track);

    mradm::SceneTrackRef lfe_track;
    lfe_track.track_uid = "ATU_00000002";
    mradm::SceneDirectSpeakersBlock lfe;
    lfe.speaker_labels = {"LFE"};
    lfe.gain = 1.0F;
    lfe.low_pass_hz = 120.0F; // marks this as LFE
    lfe_track.ds_blocks.push_back(lfe);
    bed.tracks.push_back(lfe_track);

    scene.objects.push_back(bed);
    return scene;
}

// Scene with a single HOA pack (3rd order), held in scene.hoa_tracks (not objects).
mradm::AdmScene make_hoa_scene() {
    mradm::AdmScene scene;
    scene.info.sample_rate = 48000U;
    mradm::SceneHOATracks pack;
    pack.object_id = "AO_5001";
    pack.pack_format_id = "AP_00031001";
    pack.gain = 1.0F;
    pack.mute = false;
    mradm::SceneHOAChannel ch;
    ch.track_uid = "ATU_00000001";
    ch.order = 0;
    ch.degree = 0;
    ch.blocks.push_back(mradm::SceneHOAChannelBlock{});
    pack.channels.push_back(ch);
    scene.hoa_tracks.push_back(pack);
    return scene;
}

bool apply_inline(mradm::AdmScene& scene, const std::string& json, const std::string& tag) {
    const auto path = write_temp_json("mr_semantic_" + tag + ".json", json);
    const FileGuard guard{path};
    auto policy = load_policy(path);
    if (!check(policy.has_value(), tag + ": policy parses")) {
        if (!policy) {
            std::cerr << policy.error().message << "\n";
        }
        return false;
    }
    auto result = mradm::apply_semantic_policy(scene, *policy, scene.info.sample_rate, nullptr);
    return check(result.has_value(), tag + ": policy applies");
}

bool verify_gain_and_mute() {
    bool ok = true;
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "global": { "gain": { "scale": 0.5 } } })json",
                           "gain_scale");
        ok &= check(near(scene.objects.at(0).gain, 0.5F) && near(scene.objects.at(1).gain, 0.5F),
                    "gain scale halves all object gains");
    }
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "global": { "gain": { "gain_db": -6.0205999 } } })json",
                           "gain_db");
        ok &= check(near(scene.objects.at(0).gain, 0.5F), "gain_db -6.02 dB halves object gain");
    }
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "name_glob": "*kick*", "gain": { "mute": true } } ] })json",
                           "mute");
        ok &= check(scene.objects.at(0).mute && !scene.objects.at(1).mute, "mute targets only matched object");
    }
    return ok;
}

bool verify_position_override() {
    bool ok = true;
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1001", "position": { "azimuth": 30.0, "offset": { "azimuth": 10.0 }, "lock_elevation": 45.0 } } ] })json",
                           "pos_abs_offset_lock");
        const auto& p = scene.objects.at(0).tracks.at(0).blocks.at(0).position;
        ok &= check(!p.cartesian, "position output is polar");
        ok &= check(near(p.azimuth, 40.0F), "absolute + offset azimuth (30+10)");
        ok &= check(near(p.elevation, 45.0F), "lock_elevation forces elevation");
    }
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1001", "position": { "azimuth": 30.0, "lock_azimuth": 90.0 } } ] })json",
                           "pos_lock_wins");
        ok &= check(near(scene.objects.at(0).tracks.at(0).blocks.at(0).position.azimuth, 90.0F),
                    "lock_azimuth wins over absolute");
    }
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1001", "position": { "azimuth": 200.0, "elevation": 200.0 } } ] })json",
                           "pos_clamp_wrap");
        const auto& p = scene.objects.at(0).tracks.at(0).blocks.at(0).position;
        ok &= check(near(p.azimuth, -160.0F), "azimuth wraps to [-180,180]");
        ok &= check(near(p.elevation, 90.0F), "elevation clamps to 90");
    }
    {
        // A real position change on a Cartesian block converts it to polar.
        auto scene = make_scene();
        auto& blk = scene.objects.at(0).tracks.at(0).blocks.at(0);
        blk.position.cartesian = true;
        blk.position.x = 0.0F;
        blk.position.y = 1.0F; // front → azimuth 0
        blk.position.z = 0.0F;
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1001", "position": { "offset": { "azimuth": 10.0 } } } ] })json",
                           "pos_cartesian");
        ok &= check(!blk.position.cartesian, "cartesian block converted to polar on real change");
        ok &= check(near(blk.position.azimuth, 10.0F), "front cartesian az 0 + offset 10 = 10");
    }
    {
        // A zero-offset position policy must leave a Cartesian block untouched.
        auto scene = make_scene();
        auto& blk = scene.objects.at(0).tracks.at(0).blocks.at(0);
        blk.position.cartesian = true;
        blk.position.x = 0.2F;
        blk.position.y = 0.5F;
        blk.position.z = 0.1F;
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1001", "position": { "offset": { "azimuth": 0.0, "elevation": 0.0, "distance": 0.0 } } } ] })json",
                           "pos_zero_noop");
        ok &= check(blk.position.cartesian, "zero-offset leaves block Cartesian");
        ok &= check(near(blk.position.x, 0.2F) && near(blk.position.y, 0.5F) && near(blk.position.z, 0.1F),
                    "zero-offset leaves coordinates unchanged");
    }
    return ok;
}

bool verify_match_dimensions() {
    bool ok = true;
    // all → both muted.
    {
        auto scene = make_rich_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "all": true, "gain": { "mute": true } } ] })json",
                           "match_all");
        ok &= check(scene.objects.at(0).mute && scene.objects.at(1).mute, "all matches every object");
    }
    // importance_max:5 → only low-importance ambience (2), not dialogue (9).
    {
        auto scene = make_rich_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "importance_max": 5, "gain": { "mute": true } } ] })json",
                           "match_importance");
        ok &=
            check(!scene.objects.at(0).mute && scene.objects.at(1).mute, "importance_max matches low-importance only");
    }
    // dialogue_id:1 → only dialogue.
    {
        auto scene = make_rich_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "dialogue_id": 1, "gain": { "mute": true } } ] })json",
                           "match_dialogue");
        ok &= check(scene.objects.at(0).mute && !scene.objects.at(1).mute, "dialogue_id matches dialogue only");
    }
    // content by name, programme by id → only the dialogue object (member of both).
    {
        auto scene = make_rich_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "content": "Main Dialogue", "gain": { "mute": true } } ] })json",
                           "match_content");
        ok &= check(scene.objects.at(0).mute && !scene.objects.at(1).mute, "content name matches its member object");
    }
    {
        auto scene = make_rich_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "programme": "APR_1001", "gain": { "mute": true } } ] })json",
                           "match_programme");
        ok &= check(scene.objects.at(0).mute && !scene.objects.at(1).mute, "programme id matches its member object");
    }
    return ok;
}

bool verify_channel_lock_full() {
    bool ok = true;
    // Force-enable + set max_distance on a block that has channel_lock off.
    {
        auto scene = make_scene();
        scene.objects.at(1).tracks.at(0).blocks.at(0).channel_lock = false;
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1002", "channel_lock": { "enabled": true, "max_distance": 0.3 } } ] })json",
                           "cl_enable");
        const auto& b = scene.objects.at(1).tracks.at(0).blocks.at(0);
        ok &= check(b.channel_lock, "channel_lock force-enabled");
        ok &= check(b.channel_lock_max_distance.has_value() && near(*b.channel_lock_max_distance, 0.3F),
                    "channel_lock max_distance set");
    }
    // Disabling still clears max_distance (no regression).
    {
        auto scene = make_scene(); // kick has channel_lock=true, max_distance=0.5
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_1001", "channel_lock": { "enabled": false } } ] })json",
                           "cl_disable");
        const auto& b = scene.objects.at(0).tracks.at(0).blocks.at(0);
        ok &= check(!b.channel_lock && !b.channel_lock_max_distance.has_value(), "disable clears max_distance");
    }
    return ok;
}

bool verify_direct_speakers() {
    bool ok = true;
    // DS block gain: scale + gain_db.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "gain": { "scale": 0.5 } } } ] })json",
                           "ds_gain_scale");
        ok &= check(near(scene.objects.at(0).tracks.at(0).ds_blocks.at(0).gain, 0.5F) &&
                        near(scene.objects.at(0).tracks.at(1).ds_blocks.at(0).gain, 0.5F),
                    "ds gain scale applies to all DS blocks");
    }
    // DS mute → ds.gain = 0.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "gain": { "mute": true } } } ] })json",
                           "ds_mute");
        ok &= check(near(scene.objects.at(0).tracks.at(0).ds_blocks.at(0).gain, 0.0F), "ds mute zeroes gain");
    }
    // speaker_label filter: only M+000 changes, LFE untouched.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "speaker_label": "M+000", "gain": { "scale": 0.25 } } } ] })json",
                           "ds_label");
        ok &= check(near(scene.objects.at(0).tracks.at(0).ds_blocks.at(0).gain, 0.25F), "label filter hits M+000");
        ok &= check(near(scene.objects.at(0).tracks.at(1).ds_blocks.at(0).gain, 1.0F), "label filter spares LFE");
    }
    // lfe filter: lfe:true only LFE; lfe:false only non-LFE.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "lfe": true, "gain": { "mute": true } } } ] })json",
                           "ds_lfe_true");
        ok &= check(near(scene.objects.at(0).tracks.at(0).ds_blocks.at(0).gain, 1.0F), "lfe:true spares non-LFE");
        ok &= check(near(scene.objects.at(0).tracks.at(1).ds_blocks.at(0).gain, 0.0F), "lfe:true mutes LFE");
    }
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "lfe": false, "gain": { "scale": 0.5 } } } ] })json",
                           "ds_lfe_false");
        ok &= check(near(scene.objects.at(0).tracks.at(0).ds_blocks.at(0).gain, 0.5F), "lfe:false hits non-LFE");
        ok &= check(near(scene.objects.at(0).tracks.at(1).ds_blocks.at(0).gain, 1.0F), "lfe:false spares LFE");
    }
    // AND filter: speaker_label + lfe must both match.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "speaker_label": "M+000", "lfe": true, "gain": { "scale": 0.1 } } } ] })json",
                           "ds_and");
        ok &= check(near(scene.objects.at(0).tracks.at(0).ds_blocks.at(0).gain, 1.0F) &&
                        near(scene.objects.at(0).tracks.at(1).ds_blocks.at(0).gain, 1.0F),
                    "AND filter (M+000 && lfe) matches nothing");
    }
    // Position re-aim on a DS block, sets has_position.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "direct_speakers": { "speaker_label": "M+000", "position": { "azimuth": 30.0 } } } ] })json",
                           "ds_reaim");
        const auto& c = scene.objects.at(0).tracks.at(0).ds_blocks.at(0);
        ok &= check(c.has_position && near(c.azimuth, 30.0F), "ds position re-aim sets azimuth + has_position");
    }
    // Independent DS rules must NOT cross-contaminate filters: a global rule
    // (M+000 only) + an object rule (LFE only) each apply with their own filter.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "global": { "direct_speakers": { "speaker_label": "M+000", "gain": { "scale": 0.5 } } },
  "objects": [ { "id": "AO_4001", "direct_speakers": { "lfe": true, "gain": { "mute": true } } } ] })json",
                           "ds_no_cross_contam");
        const auto& center = scene.objects.at(0).tracks.at(0).ds_blocks.at(0); // M+000
        const auto& lfe = scene.objects.at(0).tracks.at(1).ds_blocks.at(0);    // LFE
        ok &= check(near(center.gain, 0.5F), "global M+000 rule halves center independently");
        ok &= check(near(lfe.gain, 0.0F), "object LFE rule mutes LFE independently");
    }
    return ok;
}

bool verify_head_locked() {
    bool ok = true;

    // Top-level head_locked reaches every Objects block. Object rules are
    // applied after global, and later object rules keep last-write-wins.
    {
        auto scene = make_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "global": { "head_locked": true },
  "objects": [
    { "id": "AO_1001", "head_locked": false },
    { "id": "AO_1002", "head_locked": false },
    { "id": "AO_1002", "head_locked": true }
  ] })json",
                           "head_locked_objects");
        ok &= check(!scene.objects.at(0).tracks.at(0).blocks.at(0).head_locked,
                    "object head_locked=false overrides global true");
        ok &= check(scene.objects.at(1).tracks.at(0).blocks.at(0).head_locked, "later object head_locked rule wins");
    }

    // The filtered DirectSpeakers value is applied after the top-level value,
    // so an explicit false can unlock one channel without changing the rest.
    {
        auto scene = make_ds_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_4001", "head_locked": true,
    "direct_speakers": { "speaker_label": "M+000", "head_locked": false } } ] })json",
                           "head_locked_ds_filter");
        ok &= check(!scene.objects.at(0).tracks.at(0).ds_blocks.at(0).head_locked,
                    "filtered DS head_locked=false overrides object-level policy");
        ok &= check(scene.objects.at(0).tracks.at(1).ds_blocks.at(0).head_locked,
                    "unmatched DS channel keeps top-level head_locked=true");
    }

    // HOA policy is block-scoped just like Objects and DirectSpeakers.
    {
        auto scene = make_hoa_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "pack_format": "AP_00031001", "head_locked": true } ] })json",
                           "head_locked_hoa");
        ok &= check(scene.hoa_tracks.at(0).channels.at(0).blocks.at(0).head_locked,
                    "HOA head_locked policy reaches channel blocks");
    }

    // Booleans are strict; strings such as "false" must not be accepted.
    {
        const auto path = write_temp_json("mr_semantic_head_locked_bad.json",
                                          R"json({"schema":"mradm.semantic-policy.v1",
  "global": { "head_locked": "false" } })json");
        const FileGuard guard{path};
        ok &= check(!load_policy(path).has_value(), "non-boolean head_locked is rejected");
    }

    return ok;
}

bool verify_hoa() {
    bool ok = true;
    // id (== object_id) match + gain scale.
    {
        auto scene = make_hoa_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "id": "AO_5001", "gain": { "scale": 0.5 } } ] })json",
                           "hoa_id_gain");
        ok &= check(near(scene.hoa_tracks.at(0).gain, 0.5F), "hoa id match halves pack gain");
    }
    // pack_format match + mute.
    {
        auto scene = make_hoa_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "pack_format": "AP_00031001", "gain": { "mute": true } } ] })json",
                           "hoa_pack_mute");
        ok &= check(scene.hoa_tracks.at(0).mute, "hoa pack_format match mutes pack");
    }
    // all match + gain_db (-6.02 dB ~= x0.5).
    {
        auto scene = make_hoa_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "all": true, "gain": { "gain_db": -6.0205999 } } ] })json",
                           "hoa_all_db");
        ok &= check(near(scene.hoa_tracks.at(0).gain, 0.5F), "hoa all match applies gain_db");
    }
    // global gain also reaches HOA packs.
    {
        auto scene = make_hoa_scene();
        ok &= apply_inline(scene,
                           R"json({"schema":"mradm.semantic-policy.v1",
  "global": { "gain": { "scale": 0.25 } } })json",
                           "hoa_global");
        ok &= check(near(scene.hoa_tracks.at(0).gain, 0.25F), "global gain reaches HOA pack");
    }
    // A HOA-only rule (pack_format) must not be reported as unmatched.
    {
        auto scene = make_hoa_scene();
        const auto path = write_temp_json("mr_semantic_hoa_warn.json", R"json({"schema":"mradm.semantic-policy.v1",
  "objects": [ { "pack_format": "AP_00031001", "gain": { "scale": 0.9 } } ] })json");
        const FileGuard guard{path};
        auto policy = load_policy(path);
        ok &= check(policy.has_value(), "hoa-only rule parses");
        std::vector<std::string> warnings;
        if (policy) {
            (void) mradm::apply_semantic_policy(scene, *policy, scene.info.sample_rate, &warnings);
        }
        ok &= check(warnings.empty(), "hoa-only rule is not flagged unmatched");
    }
    return ok;
}

// Frozen copy of the C++ policy arithmetic before it moved to Rust (PCM consistency 1.2).
// The shared transforms must reproduce it bit for bit. The one intended change is the dB
// conversion: ADR 0018 replaces the platform powf with libm's, whose bits are listed here.
namespace original {

float db_to_gain(float db) {
    constexpr std::array<std::pair<float, std::uint32_t>, 7> portable{{{-96.0F, 0x3784F34FU},
                                                                       {-29.845F, 0x3D03DBECU},
                                                                       {-20.5F, 0x3DC157FBU},
                                                                       {-0.1F, 0x3F7D11D1U},
                                                                       {0.0F, 0x3F800000U},
                                                                       {3.3F, 0x3FBB289FU},
                                                                       {18.0F, 0x40FE2F5EU}}};
    // Iterator is a pointer on libc++ and a checked wrapper on MSVC.
    const auto found = // NOLINT(readability-qualified-auto)
        std::ranges::find(portable, db, &std::pair<float, std::uint32_t>::first);
    return found != portable.end() ? std::bit_cast<float>(found->second) : std::nanf("");
}

float gain(float value, const mradm::GainPolicy& policy) {
    if (policy.scale) {
        value *= *policy.scale;
    }
    if (policy.gain_db) {
        value *= db_to_gain(*policy.gain_db);
    }
    return value;
}

bool position_changes(const mradm::PositionPolicy& pos) {
    const auto nonzero = [](const std::optional<float>& v) { return v.has_value() && (*v > 0.0F || *v < 0.0F); };
    const bool offset_changes =
        pos.offset && (nonzero(pos.offset->azimuth) || nonzero(pos.offset->elevation) || nonzero(pos.offset->distance));
    return pos.azimuth || pos.elevation || pos.distance || pos.lock_azimuth || pos.lock_elevation || offset_changes;
}

void polar(float& azimuth, float& elevation, float& distance, const mradm::PositionPolicy& pos) {
    azimuth = pos.azimuth.value_or(azimuth);
    elevation = pos.elevation.value_or(elevation);
    distance = pos.distance.value_or(distance);
    if (pos.offset) {
        azimuth += pos.offset->azimuth.value_or(0.0F);
        elevation += pos.offset->elevation.value_or(0.0F);
        distance += pos.offset->distance.value_or(0.0F);
    }
    azimuth = pos.lock_azimuth.value_or(azimuth);
    elevation = pos.lock_elevation.value_or(elevation);
    azimuth = mradm::wrap_azimuth(azimuth);
    elevation = std::clamp(elevation, -90.0F, 90.0F);
    distance = std::max(0.0F, distance);
}

void block(mradm::SceneObjectBlock& b, const mradm::SemanticPolicyOverride& policy, uint32_t sample_rate) {
    if (policy.diffuse) {
        if (policy.diffuse->enabled == false) {
            b.diffuse = 0.0F;
        } else {
            b.diffuse *= policy.diffuse->scale.value_or(1.0F);
            b.diffuse = std::clamp(b.diffuse, 0.0F, policy.diffuse->max.value_or(1.0F));
        }
    }
    if (policy.extent) {
        const float scale = policy.extent->scale.value_or(1.0F);
        b.width *= scale * policy.extent->width_scale.value_or(1.0F);
        b.height *= scale * policy.extent->height_scale.value_or(1.0F);
        b.depth *= scale * policy.extent->depth_scale.value_or(1.0F);
        const float max_value = policy.extent->max.value_or(1.0F);
        b.width = std::clamp(b.width, 0.0F, max_value);
        b.height = std::clamp(b.height, 0.0F, max_value);
        b.depth = std::clamp(b.depth, 0.0F, max_value);
    }
    if (policy.divergence) {
        b.divergence = std::clamp(b.divergence * policy.divergence->scale.value_or(1.0F), 0.0F, 1.0F);
        const float range_scale = policy.divergence->range_scale.value_or(1.0F);
        b.divergence_azimuth_range =
            std::clamp(b.divergence_azimuth_range * range_scale, 0.0F, policy.divergence->max_range.value_or(120.0F));
        b.divergence_position_range = std::max(0.0F, b.divergence_position_range * range_scale);
    }
    if (policy.position && position_changes(*policy.position)) {
        auto p = mradm::scene_position_to_polar(b.position);
        p.cartesian = false;
        polar(p.azimuth, p.elevation, p.distance, *policy.position);
        b.position = p;
    }
    const uint32_t max_ms = policy.interpolation ? policy.interpolation->max_ms.value_or(0U) : 0U;
    if (max_ms > 0U && b.interp_length_samples) {
        b.interp_length_samples =
            std::min(*b.interp_length_samples, (static_cast<uint64_t>(sample_rate) * max_ms) / 1000U);
    }
}

} // namespace original

bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool same_block(const mradm::SceneObjectBlock& a, const mradm::SceneObjectBlock& b) {
    const auto& p = a.position;
    const auto& q = b.position;
    return p.cartesian == q.cartesian && same_bits(p.azimuth, q.azimuth) && same_bits(p.elevation, q.elevation) &&
           same_bits(p.distance, q.distance) && same_bits(p.x, q.x) && same_bits(p.y, q.y) && same_bits(p.z, q.z) &&
           same_bits(a.diffuse, b.diffuse) && same_bits(a.width, b.width) && same_bits(a.height, b.height) &&
           same_bits(a.depth, b.depth) && same_bits(a.divergence, b.divergence) &&
           same_bits(a.divergence_azimuth_range, b.divergence_azimuth_range) &&
           same_bits(a.divergence_position_range, b.divergence_position_range) &&
           a.interp_length_samples == b.interp_length_samples;
}

// NOLINTNEXTLINE(readability-function-size)
bool verify_numeric_matches_original() {
    bool ok = true;
    const std::array<std::optional<float>, 5> scales{std::nullopt, 0.0F, 0.37F, 1.0F, 2.75F};
    const std::array<std::optional<float>, 8> decibels{
        std::nullopt, -96.0F, -29.845F, -20.5F, -0.1F, 0.0F, 3.3F, 18.0F};
    std::size_t cases = 0;
    for (const auto& scale : scales) {
        for (const auto& gain_db : decibels) {
            mradm::SemanticPolicyOverride policy;
            policy.gain = mradm::GainPolicy{scale, gain_db, std::nullopt};
            mradm::SceneObject object;
            object.gain = 0.83F;
            object.user_level.gain_multiplier = 1.2F;
            mradm::apply_resolved_semantic_object(object, policy);
            ok &= check(same_bits(object.gain, original::gain(0.83F, *policy.gain)) &&
                            same_bits(object.user_level.gain_multiplier, original::gain(1.2F, *policy.gain)),
                        "numeric: object gain matches the original arithmetic");

            mradm::SceneDirectSpeakersBlock ds;
            ds.gain = 0.61F;
            mradm::DirectSpeakersPolicy rule;
            rule.gain = policy.gain;
            mradm::apply_resolved_semantic_direct_speaker(ds, std::span{&rule, 1U}, {});
            ok &= check(same_bits(ds.gain, original::gain(0.61F, *policy.gain)) &&
                            same_bits(ds.user_level.gain_multiplier, original::gain(1.0F, *policy.gain)),
                        "numeric: DirectSpeakers gain matches the original arithmetic");
            ++cases;
        }
    }

    const std::array<std::optional<float>, 4> offsets{std::nullopt, -0.0F, -47.25F, 361.5F};
    const std::array<mradm::SceneBlockPosition, 3> positions{
        mradm::SceneBlockPosition{true, 0.0F, 0.0F, 1.0F, -0.3F, 0.8F, 0.25F},
        mradm::SceneBlockPosition{true, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -0.0F},
        mradm::SceneBlockPosition{false, -179.5F, 80.0F, 0.6F, 0.0F, 0.0F, 0.0F}};
    for (const auto& start : positions) {
        for (const auto& offset : offsets) {
            for (const auto& absolute : offsets) {
                mradm::SemanticPolicyOverride policy;
                mradm::PositionPolicy position;
                position.azimuth = absolute;
                position.offset = mradm::PositionPolicy::Offset{offset, offset, offset};
                if (absolute && *absolute > 300.0F) {
                    position.lock_elevation = -100.0F;
                }
                policy.position = position;
                // Validated policies reject negative scales and limits; -0.0 passes validation.
                const auto magnitude = [](std::optional<float> v) { return v && *v < 0.0F ? -*v : v; };
                policy.diffuse = mradm::DiffusePolicy{std::nullopt, magnitude(absolute), magnitude(offset)};
                policy.extent =
                    mradm::ExtentPolicy{std::nullopt, 1.5F, magnitude(offset), 0.3F, magnitude(absolute), 0.9F};
                policy.divergence = mradm::DivergencePolicy{std::nullopt, magnitude(offset), 1.9F, magnitude(absolute)};
                policy.interpolation = mradm::InterpolationPolicy{std::nullopt, 7U};

                mradm::SceneObjectBlock block;
                block.position = start;
                block.diffuse = 0.7F;
                block.width = 0.4F;
                block.height = 0.2F;
                block.depth = 0.9F;
                block.divergence = 0.6F;
                block.divergence_azimuth_range = 70.0F;
                block.divergence_position_range = 0.35F;
                block.interp_length_samples = 1000U;
                auto expected = block;
                original::block(expected, policy, 44100U);
                mradm::apply_resolved_semantic_object_block(block, policy, 44100U);
                ok &= check(same_block(block, expected), "numeric: Objects block matches the original arithmetic");

                mradm::SceneDirectSpeakersBlock ds;
                ds.azimuth = start.azimuth;
                ds.elevation = start.elevation;
                ds.distance = start.distance;
                mradm::DirectSpeakersPolicy rule;
                rule.position = position;
                float az = ds.azimuth;
                float el = ds.elevation;
                float dist = ds.distance;
                const bool moves = original::position_changes(position);
                if (moves) {
                    original::polar(az, el, dist, position);
                }
                mradm::apply_resolved_semantic_direct_speaker(ds, std::span{&rule, 1U}, {});
                ok &=
                    check(ds.has_position == moves && ds.user_position_override == moves && same_bits(ds.azimuth, az) &&
                              same_bits(ds.elevation, el) && same_bits(ds.distance, dist),
                          "numeric: DirectSpeakers re-aim matches the original arithmetic");
                ++cases;
            }
        }
    }
    ok &= check(cases == 88U, "numeric: every grid case ran");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_policy_parse_and_apply();
    ok &= verify_invalid_policy_rejected();
    ok &= verify_policy_id_matching_is_case_insensitive();
    ok &= verify_gain_and_mute();
    ok &= verify_position_override();
    ok &= verify_match_dimensions();
    ok &= verify_channel_lock_full();
    ok &= verify_direct_speakers();
    ok &= verify_head_locked();
    ok &= verify_hoa();
    ok &= verify_numeric_matches_original();
    if (ok) {
        std::cout << "semantic policy test passed\n";
        return 0;
    }
    return 1;
}
