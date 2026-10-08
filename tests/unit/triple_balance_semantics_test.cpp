#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_triple_balance.h"
#include "adm/semantic_policy.h"

#include "semantics.h"
#include "size_processor.h"

namespace {
class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

  private:
    std::filesystem::path path_;
};

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

mradm::RenderPlan make_plan(uint64_t rtime) {
    mradm::RenderPlan plan;
    plan.output_layout = "9.1.6";
    plan.scene.info.sample_rate = 48000;
    plan.scene.info.num_channels = 1;
    plan.scene.info.num_frames = 52224;
    mradm::SceneObject object;
    object.id = "AO_1001";
    object.gain = 0;
    object.mute = true;
    object.end_sample = 4000;
    object.adm_source = mradm::SceneObjectSource{};
    object.adm_source->gain = {true, false, 0, 0};
    object.adm_source->mute = true;
    object.adm_source->mute_present = true;
    object.adm_source->start_samples = 1000;
    object.adm_source->absolute_start_samples = 1000;
    mradm::SceneTrackRef track;
    track.channel_index = 0;
    track.track_uid = "ATU_00000001";
    mradm::SceneObjectBlock block;
    block.position.cartesian = true;
    block.position.x = 0.23F;
    block.position.y = 0.51F;
    block.position.z = 0.37F;
    block.width = block.height = block.depth = 0.25F;
    block.start_sample = rtime + 1000;
    block.end_sample = rtime + 2000;
    block.gain = 0.25F;
    block.adm_source = mradm::SceneBlockSource{{true, false, .25, .25F}, true, rtime, 1000};
    track.blocks.push_back(block);
    object.tracks.push_back(track);
    plan.scene.objects.push_back(object);
    return plan;
}

std::vector<float> read_floats(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto bytes = input.tellg();
    if (bytes <= 0) {
        return {};
    }
    std::vector<float> output(static_cast<std::size_t>(bytes) / sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), bytes);
    return output;
}

bool dense_event_semantics() {
    auto plan = make_plan(0);
    plan.scene.info.num_frames = 170496;
    auto& blocks = plan.scene.objects.front().tracks.front().blocks;
    const auto seed = blocks.front();
    if (!seed.adm_source) {
        return check(false, "dense fixture retains source metadata");
    }
    blocks.clear();
    // A real ADM can contain several distinct, ordered positions within this control block.
    for (uint64_t frame : {0U, 169472U, 169498U, 169498U, 169573U, 169984U}) {
        auto block = seed;
        block.start_sample = frame + 1000;
        auto source = *seed.adm_source;
        source.rtime_samples = frame;
        block.adm_source = source;
        block.position.x = .1F * static_cast<float>(blocks.size());
        blocks.push_back(block);
    }
    std::string report;
    const auto scene = mradm::triple_balance::prepare_semantics(plan, report);
    if (!check(scene.has_value(), "ordered dense metadata and equal timestamps are supported")) {
        return false;
    }
    const auto diagnostic = nlohmann::json::parse(report);
    const auto& events = diagnostic["objects"][0]["tracks"][0]["events"];
    bool ok = check(diagnostic["control_event_policy"] == "last_event_in_block",
                    "report declares the file control-target selection rule");
    ok &= check(events.size() == 6 && scene->objects.front().tracks.front().blocks.size() == 6,
                "preparation retains every source event for validation and reporting");
    for (std::size_t i = 0; i < 6; ++i) {
        ok &= check(events[i]["control_target"] == (i == 0 || i >= 4),
                    "only the last source event in each control block supplies its target");
        const auto& block = blocks[i];
        ok &= check(block.adm_source && block.start_sample == block.adm_source->rtime_samples + 1000,
                    "dense preparation leaves source metadata untouched");
    }
    auto backwards = *seed.adm_source;
    backwards.rtime_samples = 169497;
    blocks[3].adm_source = backwards;
    ok &= check(!mradm::triple_balance::prepare_semantics(plan, report), "backwards metadata remains rejected");
    return ok;
}

bool point_half_code(const std::filesystem::path& fixtures) {
    std::ifstream file(fixtures / "point-motion-half-code.json");
    const auto reference = nlohmann::json::parse(file);
    const auto prefix = std::filesystem::temp_directory_path() /
                        ("mr_room_half_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto input_path = prefix.string() + "-input.wav";
    const auto output_path = prefix.string() + "-output.wav";
    FileGuard input_guard{input_path};
    FileGuard output_guard{output_path};
    auto plan = make_plan(0);
    plan.input_path = input_path;
    plan.output_path = output_path;
    plan.scene.info.num_frames = 145000;
    plan.render_window = mradm::RenderWindow{120000, 24000};
    auto& blocks = plan.scene.objects.front().tracks.front().blocks;
    blocks.front().width = blocks.front().height = blocks.front().depth = 0;
    auto second = blocks.front();
    second.position.x = -.2F;
    second.position.y = .8F;
    second.position.z = .5F;
    if (!second.adm_source) {
        return false;
    }
    second.adm_source->rtime_samples = reference["rtime_target"];
    second.start_sample = second.adm_source->rtime_samples + 1000;
    blocks.push_back(second);
    constexpr float amplitude = .0625F;
    const std::vector<float> input(plan.scene.info.num_frames, amplitude);
    {
        auto writer = mradm::audio::FloatWavWriter::open(plan.input_path, 1, 48000);
        if (!writer || writer->write(input.data(), input.size()) != input.size()) {
            return false;
        }
    }
    auto renderer = mradm::create_triple_balance_renderer();
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    const auto prepared = renderer->prepare(plan, logs);
    if (!prepared || !renderer->render_window(**prepared, plan, progress, logs)) {
        return false;
    }
    auto reader = mradm::audio::FloatWavReader::open(plan.output_path);
    if (!reader || reader->frame_count() != 24000 || reader->channels() != 16) {
        return false;
    }
    std::vector<float> output(std::size_t{24000} * 16);
    if (reader->read(output.data(), 24000) != 24000) {
        return false;
    }
    float error = 0;
    for (std::size_t channel = 0; channel < 16; ++channel) {
        const auto expected = reference["signed_gains"][channel].get<float>();
        error = std::max(error, std::fabs((output[channel] / amplitude) - expected));
    }
    std::cout << "point half-code captured gain error: " << error << '\n';
    return check(error < 2e-4F, "point motion retains reference float state below the Z=0.5 quantization boundary");
}
} // namespace

// NOLINTNEXTLINE(readability-function-size): semantic decisions and captured PCM share one small fixture.
int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    bool ok = dense_event_semantics();
    std::string report;
    for (const uint64_t first : {0U, 31U, 32U, 511U, 512U, 513U, 48000U}) {
        const auto original = make_plan(first);
        auto scene = mradm::triple_balance::prepare_semantics(original, report);
        if (!check(scene.has_value(), "measured initial event and native zero gain/mute are supported")) {
            return 1;
        }
        const auto& object = scene->objects.front();
        const auto& track = object.tracks.front();
        ok &= check(original.scene.objects.front().mute && original.scene.objects.front().gain == 0,
                    "compatibility preparation preserves the source scene");
        ok &= check(!object.mute && object.gain == 1 && object.end_sample == original.scene.info.num_frames,
                    "native zero gain/mute/short duration do not gate reference PCM");
        ok &= check(track.blocks.front().start_sample == 0, "absolute timeline starts at file zero");
        ok &= check(track.blocks.size() == (first >= 512 ? 2U : 1U),
                    "default state is confined to preceding control blocks");
        const auto diagnostic = nlohmann::json::parse(report);
        ok &= check(diagnostic["objects"][0]["tracks"][0]["events"][0]["source_absolute_start_sample"] == first + 1000,
                    "diagnostics retain source absolute time before ignoring object start");
    }
    auto plan = make_plan(48000);
    mradm::SemanticPolicyOverride user;
    user.gain = mradm::GainPolicy{.scale = .25F, .gain_db = 6.0206F, .mute = false};
    mradm::apply_resolved_semantic_object(plan.scene.objects.front(), user);
    auto scene = mradm::triple_balance::prepare_semantics(plan, report);
    ok &= check(scene && std::fabs(mradm::triple_balance::user_output_gain(scene->objects.front()) - .5F) < 1e-6F,
                "user level is an independent multiplier even with authored gain zero");
    user.gain = mradm::GainPolicy{.mute = true};
    mradm::apply_resolved_semantic_object(plan.scene.objects.front(), user);
    scene = mradm::triple_balance::prepare_semantics(plan, report);
    ok &= check(scene && !scene->objects.front().mute &&
                    mradm::triple_balance::user_output_gain(scene->objects.front()) == 0,
                "user mute suppresses output without skipping object state");
    auto invalid = make_plan(0);
    mradm::SceneObjectSource nested_source;
    nested_source.has_parent = true;
    invalid.scene.objects.front().adm_source = nested_source;
    ok &= check(!mradm::triple_balance::prepare_semantics(invalid, report), "nested source objects remain unsupported");
    invalid = make_plan(0);
    invalid.scene.objects.push_back(invalid.scene.objects.front());
    ok &= check(!mradm::triple_balance::prepare_semantics(invalid, report), "shared PCM bindings remain unsupported");
    invalid = make_plan(0);
    invalid.scene.info.sample_rate = 44100;
    ok &= check(!mradm::triple_balance::prepare_semantics(invalid, report),
                "extended semantics require the verified rate");

    auto with_bed = make_plan(0);
    with_bed.scene.objects.front().tracks.front().channel_index = 10;
    mradm::SceneObject bed;
    bed.id = "AO_1002";
    bed.adm_source = mradm::SceneObjectSource{};
    const std::vector<std::string> labels{
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
    for (std::size_t index = 0; index < labels.size(); ++index) {
        mradm::SceneTrackRef bed_track;
        bed_track.channel_index = static_cast<uint16_t>(index);
        bed_track.track_uid = "ATU_bed_" + std::to_string(index);
        bed_track.ds_blocks.emplace_back();
        bed_track.ds_blocks.front().speaker_labels.push_back(labels[index]);
        bed.tracks.push_back(bed_track);
    }
    user.gain = mradm::GainPolicy{.scale = .5F, .mute = false};
    mradm::apply_resolved_semantic_object(bed, user);
    with_bed.scene.objects.push_back(bed);
    with_bed.scene.info.num_channels = 11;
    scene = mradm::triple_balance::prepare_semantics(with_bed, report);
    ok &= check(scene && scene->objects.back().gain == 1 &&
                    mradm::triple_balance::user_output_gain(scene->objects.back()) == .5F,
                "verified bed distinguishes ignored native gain from global user gain");

    const std::filesystem::path fixtures(argv[1]);
    ok &= point_half_code(fixtures);
    const auto input = read_floats(fixtures / "input.f32");
    if (!check(input.size() == 52224, "complete late-first-event input fixture")) {
        return 1;
    }
    for (const auto* layout : {"7.1.4", "9.1.6"}) {
        plan = make_plan(48000);
        plan.output_layout = layout;
        scene = mradm::triple_balance::prepare_semantics(plan, report);
        std::vector<mradm::triple_balance::SizeEvent> events;
        std::ranges::transform(
            scene->objects.front().tracks.front().blocks, std::back_inserter(events), [](const auto& block) {
                return mradm::triple_balance::SizeEvent{
                    block.start_sample,
                    {(block.position.x + 1) * .5F, (1 - block.position.y) * .5F, block.position.z},
                    block.width};
            });
        auto processor = mradm::triple_balance::SizeObjectProcessor::create(events, layout, 48000);
        if (!check(processor.has_value(), "compiled semantic timeline initializes DSP")) {
            return 1;
        }
        const auto expected = read_floats(fixtures / (std::string(layout) + ".f32"));
        const auto channels = processor->channel_count();
        if (!check(expected.size() == 5120 * channels, "complete reference transition excerpt")) {
            return 1;
        }
        std::vector<float> baseline;
        for (const std::size_t chunk : {1U, 31U, 32U, 257U, 511U, 512U, 513U, 1024U}) {
            processor->reset();
            std::vector<float> output;
            for (std::size_t at = 0; at < input.size(); at += chunk) {
                ok &= check(processor->push(std::span(input).subspan(at, std::min(chunk, input.size() - at)), output)
                                .has_value(),
                            "chunked semantic PCM renders");
            }
            ok &= check(processor->finish(output).has_value(), "semantic PCM finishes");
            if (baseline.empty()) {
                baseline = output;
            }
            ok &= check(output == baseline, "late initial metadata is independent of caller block size");
        }
        float error = 0;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            error = std::max(error, std::fabs(expected[i] - baseline[(47104 * channels) + i]));
        }
        std::cout << layout << " captured semantic transition max error: " << error << '\n';
        ok &= check(error < 1e-4F, "compiled default/late event matches actual Renderer PCM");
    }
    return ok ? 0 : 1;
}
