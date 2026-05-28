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

} // namespace

int main() {
    bool ok = true;
    ok &= verify_policy_parse_and_apply();
    ok &= verify_invalid_policy_rejected();
    ok &= verify_policy_id_matching_is_case_insensitive();
    ok &= verify_semantic_report();
    ok &= verify_semantic_template_round_trips();
    if (ok) {
        std::cout << "semantic policy test passed\n";
        return 0;
    }
    return 1;
}
