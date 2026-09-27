#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "adm/render.h"
#include "adm/semantic_policy.h"

#include "room_compat_bed.h"
#include "room_compat_semantics.h"

namespace {
bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

mradm::RenderPlan plan_from(const nlohmann::json& fixture) {
    mradm::RenderPlan plan;
    plan.output_layout = "9.1.6";
    plan.scene.info = {48000, 10, 240000, "", mradm::SceneSourceKind::adm, "", {}};
    mradm::SceneObject bed;
    bed.id = "bed";
    bed.gain = 0;
    bed.mute = true;
    bed.end_sample = 96000;
    bed.adm_source = mradm::SceneObjectSource{};
    bed.adm_source->gain = {true, false, 0, 0};
    bed.adm_source->mute = true;
    bed.adm_source->start_samples = 48000;
    for (std::size_t index = 0; index < fixture["labels"].size(); ++index) {
        mradm::SceneTrackRef track;
        track.channel_index = static_cast<uint16_t>(index);
        track.track_uid = "ATU_" + std::to_string(index);
        mradm::SceneDirectSpeakersBlock block;
        block.speaker_labels.push_back(fixture["labels"][index]);
        block.gain = 0;
        block.start_sample = 96000;
        block.end_sample = 144000;
        block.adm_source = mradm::SceneBlockSource{{true, false, 0, 0}, true, 48000, 48000};
        track.ds_blocks.push_back(block);
        block.start_sample = 144000;
        block.adm_source->rtime_samples = 96000;
        track.ds_blocks.push_back(block);
        bed.tracks.push_back(track);
    }
    plan.scene.objects.push_back(bed);
    return plan;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    std::ifstream input(argv[1]);
    const auto fixture = nlohmann::json::parse(input);
    auto plan = plan_from(fixture);
    bool ok = true;
    std::string report;
    for (const auto* layout : {"7.1.4", "9.1.6"}) {
        plan.output_layout = layout;
        const auto prepared = mradm::room_compat::prepare_semantics(plan, report);
        if (!check(prepared.has_value(), "captured bed source fields prepare successfully")) {
            return 1;
        }
        const auto& bed = prepared->objects.front();
        ok &=
            check(!bed.mute && bed.gain == 1 && bed.end_sample == 240000, "native bed gain/mute/time do not gate PCM");
        for (std::size_t index = 0; index < bed.tracks.size(); ++index) {
            const auto& blocks = bed.tracks[index].ds_blocks;
            ok &= check(blocks.size() == 1 && blocks.front().start_sample == 0 && blocks.front().gain == 1,
                        "bed source blocks compile into a constant full-file route");
            const auto gains = mradm::room_compat::bed_gains(blocks.front(), layout);
            const auto reference = fixture["layouts"][layout][index].get<std::vector<float>>();
            ok &= check(gains && *gains == reference, "bed route exactly matches captured native OAR gains");
        }
        const auto diagnostic = nlohmann::json::parse(report);
        ok &= check(diagnostic["beds"][0]["tracks"][0]["events"][0]["source_rtime_sample"] == 48000,
                    "renderer report preserves authored relative time");
    }
    mradm::SemanticPolicyOverride object_policy;
    object_policy.gain = mradm::GainPolicy{.scale = .5F, .mute = false};
    mradm::apply_resolved_semantic_object(plan.scene.objects.front(), object_policy);
    mradm::DirectSpeakersPolicy channel_policy;
    channel_policy.speaker_label = "RC_Lts";
    channel_policy.gain = mradm::GainPolicy{.scale = .25F, .gain_db = 6.0206F, .mute = false};
    const std::vector<mradm::DirectSpeakersPolicy> policies{channel_policy};
    for (auto& track : plan.scene.objects.front().tracks) {
        for (auto& block : track.ds_blocks) {
            mradm::apply_resolved_semantic_direct_speaker(block, policies, {});
        }
    }
    auto prepared = mradm::room_compat::prepare_semantics(plan, report);
    ok &=
        check(prepared &&
                  std::fabs((mradm::room_compat::bed_user_gain(prepared->objects.front().tracks[8].ds_blocks.front()) *
                             mradm::room_compat::user_output_gain(prepared->objects.front())) -
                            .25F) < 1e-6F,
              "user channel and object gain apply once after ignored native zero gain");
    const auto rejected = [&](const mradm::RenderPlan& invalid) {
        const auto result = mradm::room_compat::prepare_semantics(invalid, report);
        return !result && result.error().code == mradm::ErrorCode::unsupported &&
               nlohmann::json::parse(report)["status"] == "unsupported";
    };
    auto invalid = plan;
    invalid.scene.objects.front().tracks.pop_back();
    ok &= check(rejected(invalid), "incomplete beds are rejected");
    invalid = plan;
    invalid.scene.objects.front().tracks[0].ds_blocks[0].speaker_labels = {"RC_R"};
    ok &= check(rejected(invalid), "unverified label/PCM reorder is rejected");
    invalid = plan;
    invalid.direct_speakers_routing_mode = mradm::DirectSpeakersRoutingMode::position;
    ok &= check(rejected(invalid), "extra geometry-based routing is rejected");
    invalid = plan;
    invalid.scene.info.sample_rate = 44100;
    ok &= check(rejected(invalid), "unverified bed sample rate is rejected");
    invalid = plan;
    invalid.scene.objects.front().tracks[0].ds_blocks[0].user_position_override = true;
    ok &= check(rejected(invalid), "explicit user re-aim is not silently ignored");
    return ok ? 0 : 1;
}
