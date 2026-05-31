#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

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

    auto policy = mradm::load_semantic_policy_file(path);
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
    const auto policy = mradm::load_semantic_policy_file(path);
    bool ok = true;
    ok &= check(!policy.has_value(), "unknown field rejected");
    if (!policy) {
        ok &= check(policy.error().code == mradm::ErrorCode::invalid_argument, "invalid policy error code");
    }
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

    auto policy = mradm::load_semantic_policy_file(path);
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

bool verify_semantic_report() {
    const auto path = std::filesystem::temp_directory_path() / "mr_semantic_report_test.json";
    const FileGuard guard{path};
    const auto original = make_scene();
    auto effective = original;
    effective.objects.at(0).tracks.at(0).blocks.at(0).diffuse = 0.0F;

    mradm::SemanticPolicy policy;
    mradm::SemanticObjectRule rule;
    rule.name_glob = "*kick*";
    policy.objects.push_back(rule);

    mradm::SemanticPolicyReportOptions options;
    options.renderer = "binaural";
    options.policy_path = "policy.json";
    options.capabilities.supports_diffuse = true;
    const auto result = mradm::write_semantic_report_file(path, original, effective, &policy, options);
    bool ok = true;
    ok &= check(result.has_value(), "semantic report writes");
    ok &= check(std::filesystem::exists(path), "semantic report file exists");
    return ok;
}

bool verify_semantic_template_round_trips() {
    const auto path = std::filesystem::temp_directory_path() / "mr_semantic_template_test.json";
    const FileGuard guard{path};
    const auto scene = make_scene();
    const auto write_result = mradm::write_semantic_policy_template_file(path, scene);
    bool ok = true;
    ok &= check(write_result.has_value(), "semantic policy template writes");

    const auto policy = mradm::load_semantic_policy_file(path);
    ok &= check(policy.has_value(), "semantic policy template parses as policy");
    if (policy) {
        ok &= check(policy->objects.size() == scene.objects.size(), "semantic policy template includes all objects");
        ok &= check(policy->global.has_value(), "semantic policy template includes global defaults");
    }
    return ok;
}

// Scene with content / programme / importance / dialogue for match-dimension tests.
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

bool apply_inline(mradm::AdmScene& scene, const std::string& json, const std::string& tag) {
    const auto path = write_temp_json("mr_semantic_" + tag + ".json", json);
    const FileGuard guard{path};
    auto policy = mradm::load_semantic_policy_file(path);
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

// The generated neutral template, applied unmodified, must not change the scene
// — including muted objects, channel_lock=false blocks, and Cartesian positions.
bool verify_template_apply_is_identity() {
    const auto path = std::filesystem::temp_directory_path() / "mr_semantic_identity_test.json";
    const FileGuard guard{path};

    mradm::AdmScene scene;
    scene.info.sample_rate = 48000U;
    mradm::SceneObject obj;
    obj.id = "AO_3001";
    obj.name = "Muted Cartesian";
    obj.gain = 0.75F;
    obj.mute = true; // originally muted — must stay muted
    mradm::SceneTrackRef track;
    track.track_uid = "ATU_00000001";
    mradm::SceneObjectBlock block;
    block.channel_lock = false;      // must stay off
    block.position.cartesian = true; // must stay Cartesian
    block.position.x = 0.3F;
    block.position.y = 0.6F;
    block.position.z = -0.2F;
    track.blocks.push_back(block);
    obj.tracks.push_back(track);
    scene.objects.push_back(obj);

    // A DirectSpeakers LFE channel: must keep its gain and stay unmuted.
    mradm::SceneObject bed;
    bed.id = "AO_3002";
    bed.name = "LFE";
    mradm::SceneTrackRef bed_track;
    bed_track.track_uid = "ATU_00000002";
    mradm::SceneDirectSpeakersBlock ds;
    ds.speaker_labels = {"LFE"};
    ds.gain = 0.9F;
    ds.low_pass_hz = 120.0F;
    bed_track.ds_blocks.push_back(ds);
    bed.tracks.push_back(bed_track);
    scene.objects.push_back(bed);

    const auto write_result = mradm::write_semantic_policy_template_file(path, scene);
    bool ok = check(write_result.has_value(), "identity: template writes");
    auto policy = mradm::load_semantic_policy_file(path);
    ok &= check(policy.has_value(), "identity: template parses");
    if (!policy) {
        return false;
    }

    auto applied = scene;
    auto result = mradm::apply_semantic_policy(applied, *policy, applied.info.sample_rate, nullptr);
    ok &= check(result.has_value(), "identity: template applies");

    const auto& a = applied.objects.at(0);
    const auto& b = a.tracks.at(0).blocks.at(0);
    ok &= check(a.mute, "identity: muted object stays muted");
    ok &= check(near(a.gain, 0.75F), "identity: object gain unchanged");
    ok &= check(!b.channel_lock, "identity: channel_lock stays off");
    ok &= check(b.position.cartesian, "identity: Cartesian position stays Cartesian");
    ok &= check(near(b.position.x, 0.3F) && near(b.position.y, 0.6F) && near(b.position.z, -0.2F),
                "identity: Cartesian coordinates unchanged");
    const auto& ds_eff = applied.objects.at(1).tracks.at(0).ds_blocks.at(0);
    ok &= check(near(ds_eff.gain, 0.9F), "identity: DS LFE gain unchanged");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_policy_parse_and_apply();
    ok &= verify_invalid_policy_rejected();
    ok &= verify_policy_id_matching_is_case_insensitive();
    ok &= verify_semantic_report();
    ok &= verify_semantic_template_round_trips();
    ok &= verify_gain_and_mute();
    ok &= verify_position_override();
    ok &= verify_match_dimensions();
    ok &= verify_channel_lock_full();
    ok &= verify_direct_speakers();
    ok &= verify_template_apply_is_identity();
    if (ok) {
        std::cout << "semantic policy test passed\n";
        return 0;
    }
    return 1;
}
