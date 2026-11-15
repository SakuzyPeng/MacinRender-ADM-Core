// The room-binaural Scene stream through the C ABI (R3 C2 kept this part of the former SDK test;
// the file renders, switch rules, source growth and resets moved to Rust: mradm-engine
// room_render.rs and mradm-render room_runtime.rs). Driven by room_fixture.py, which writes the
// synthetic bundle and its variants.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "adm/c_api.h"

namespace {

constexpr const char* k_screen_json = R"({"schema":"mradm.screen-config","version":1,
    "reproduction":{"coordinates":"polar","centre":[0,0,1],"width":90,"aspect_ratio":1.78}})";

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string utf8(const std::filesystem::path& path) {
    auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// The C option setters of the screen configuration and room quality.
void option_test() {
    auto* options = adm_create_render_options();
    require(options != nullptr, "options");
    require(adm_render_options_set_screen_config_json(options, k_screen_json) == ADM_ERROR_OK, "C screen setter");
    require(adm_render_options_set_screen_config_json(options, "{}") == ADM_ERROR_INVALID_ARGUMENT,
            "C screen setter rejects malformed input");
    require(adm_render_options_set_screen_config_json(options, nullptr) == ADM_ERROR_OK, "C screen clear");
    require(adm_render_options_set_room_quality(options, ADM_ROOM_QUALITY_REALTIME) == ADM_ERROR_OK,
            "room quality setter");
    require(adm_render_options_set_room_quality(options, static_cast<adm_room_quality_t>(2)) ==
                ADM_ERROR_INVALID_ARGUMENT,
            "unknown quality rejected");
    require(adm_render_options_set_room(options, nullptr) == ADM_ERROR_OK, "clear room resource independently");
    adm_destroy_render_options(options);
}

// A loaded room and the inspection of one configuration.
struct Room {
    adm_context_t* context{adm_create_context()};
    adm_room_t* room{nullptr};
    explicit Room(const std::filesystem::path& manifest) {
        const auto path = utf8(manifest);
        require(context != nullptr && adm_room_load(context, path.c_str(), &room) == ADM_ERROR_OK, "C room load");
    }
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;
    Room(Room&&) = delete;
    Room& operator=(Room&&) = delete;
    ~Room() {
        adm_room_destroy(room);
        adm_destroy_context(context);
    }
    [[nodiscard]] nlohmann::json inspect(const std::string& hrir, double metres, adm_room_quality_t quality) const {
        adm_room_config_t config{};
        config.struct_size = sizeof(config);
        config.room = room;
        config.metres_per_unit = metres;
        config.hrir_manifest = hrir.empty() ? nullptr : hrir.c_str();
        char* report = nullptr;
        require(adm_room_inspect_with_quality(context, &config, quality, &report) == ADM_ERROR_OK, "C room inspect");
        auto json = nlohmann::json::parse(report);
        adm_free_string(report);
        return json;
    }
};

// Both qualities prepare from the same HRIR and report their processing policy.
void quality_test(const std::filesystem::path& root) {
    const Room room{root / "room.json"};
    const auto exact = room.inspect({}, 1.5, ADM_ROOM_QUALITY_EXACT);
    const auto fast = room.inspect({}, 1.5, ADM_ROOM_QUALITY_REALTIME);
    require(exact.at("quality") == "exact" && fast.at("quality") == "realtime", "qualities keep separate caches");
    require(exact.at("hrir_hash") == fast.at("hrir_hash"), "qualities use identical raw HRIR resources");
    require(exact.at("processing_policy") == "room-exact-v1" && fast.at("processing_policy") == "room-realtime-v2",
            "inspection identifies the actual processing policy");
    require(exact.at("reflection_pose_limit_degrees") == 0.0 && exact.at("reflection_pose_update_frames") == 0 &&
                fast.at("reflection_pose_limit_degrees") == 0.5 && fast.at("reflection_pose_update_frames") == 1024,
            "inspection reports the session-fixed reflection pose budget");
}

// NOLINTNEXTLINE(readability-function-size): one integration lifecycle and its assertions.
void scene_test(const std::filesystem::path& root,
                std::uint32_t output_rate,
                bool screen = false,
                const std::filesystem::path& sofa = {},
                adm_room_quality_t quality = ADM_ROOM_QUALITY_EXACT) {
    auto* context = adm_create_context();
    require(context != nullptr, "context");
    adm_room_t* room = nullptr;
    const auto manifest = utf8(root / "room.json");
    require(adm_room_load(context, manifest.c_str(), &room) == ADM_ERROR_OK, "C room load");
    adm_room_config_t room_config{};
    room_config.struct_size = sizeof(room_config);
    room_config.room = room;
    room_config.metres_per_unit = 1.5;
    const auto sofa_name = utf8(sofa);
    if (!sofa.empty()) {
        room_config.hrir_manifest = sofa_name.c_str();
    }
    char* report = nullptr;
    const auto inspect_code = quality == ADM_ROOM_QUALITY_EXACT
                                  ? adm_room_inspect(context, &room_config, &report)
                                  : adm_room_inspect_with_quality(context, &room_config, quality, &report);
    require(inspect_code == ADM_ERROR_OK, "C room inspect");
    const auto inspection = nlohmann::json::parse(report);
    adm_free_string(report);
    const auto expected_quality = quality == ADM_ROOM_QUALITY_EXACT ? "exact" : "realtime";
    require(inspection.at("quality") == expected_quality, "inspection reports the selected preparation quality");
    const auto tail = inspection.at("tail_frames").get<std::uint64_t>();
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.rendering.struct_size = sizeof(config.rendering);
    config.rendering.renderer = ADM_RENDERER_ROOM_BINAURAL;
    config.rendering.output_layout = "binaural";
    config.input_sample_rate = 48000;
    config.output_sample_rate = output_rate;
    config.output_ring_frames = 32768;
    config.startup_watermark_frames = 1;
    if (!sofa.empty() && sofa.extension() == ".sofa") {
        room_config.hrir_manifest = nullptr;
        config.rendering.sofa_path = sofa_name.c_str();
    }
    adm_scene_stream_t* stream = nullptr;
    require(adm_create_scene_stream_with_room_quality(
                context, &config, &room_config, static_cast<adm_room_quality_t>(99), nullptr, &stream) ==
                    ADM_ERROR_INVALID_ARGUMENT &&
                stream == nullptr,
            "invalid quality cannot create a Scene session");
    const auto create_code =
        quality == ADM_ROOM_QUALITY_EXACT
            ? adm_create_scene_stream_with_room_ex(
                  context, &config, &room_config, screen ? k_screen_json : nullptr, &stream)
            : adm_create_scene_stream_with_room_quality(
                  context, &config, &room_config, quality, screen ? k_screen_json : nullptr, &stream);
    require(create_code == ADM_ERROR_OK, "create Scene room stream");
    std::uint64_t unchanged_request = 0;
    char* switch_error = nullptr;
    require(adm_scene_stream_switch_room(stream, &room_config, &unchanged_request, &switch_error) == ADM_ERROR_OK,
            "existing C room switch inherits the session quality");
    adm_free_string(switch_error);
    adm_room_destroy(room); // the session retained both room and HRIR assets
    require(adm_scene_stream_begin_epoch(stream, 1, 0) == ADM_ERROR_OK, "begin epoch");
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
    descriptor.element_id = 7;
    descriptor.has_position = 1;
    descriptor.position_y = 2; // distance must survive the Scene boundary
    if (screen) {
        descriptor.position_x = -0.96961924F;
        descriptor.position_y = 1.74923944F;
    }
    require(adm_scene_stream_configure_generation(stream, 1, 1, &descriptor, 1) == ADM_ERROR_OK,
            "configure Scene real distance");
    std::array<float, 1001> samples{};
    samples[0] = 0.1F;
    samples[913] = 0.05F;
    adm_scene_pcm_plane_t plane{};
    plane.struct_size = sizeof(plane);
    plane.element_id = 7;
    plane.samples = samples.data();
    plane.sample_count = static_cast<std::uint32_t>(samples.size());
    plane.stride = 1;
    plane.has_signal = 1;
    adm_scene_initial_state_t initial{};
    initial.struct_size = sizeof(initial);
    initial.element_id = 7;
    initial.state.struct_size = sizeof(initial.state);
    initial.state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN | ADM_SCENE_STATE_POSITION;
    initial.state.active = 1;
    initial.state.linear_gain = 1;
    initial.state.position_x = descriptor.position_x;
    initial.state.position_y = descriptor.position_y;
    if (screen) {
        initial.state.valid_fields |= ADM_SCENE_STATE_SCREEN_REFERENCE;
        initial.state.screen_reference = 1;
    }
    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 1;
    frame.generation_id = 1;
    frame.duration_samples = static_cast<std::uint32_t>(samples.size());
    frame.pcm_count = 1;
    frame.pcm = &plane;
    frame.initial_state_count = 1;
    frame.initial_states = &initial;
    std::int32_t submitted{};
    require(adm_scene_stream_submit_frame(stream, &frame, 1000, &submitted) == ADM_ERROR_OK &&
                submitted == ADM_SCENE_SUBMIT_ACCEPTED,
            "submit Scene room frame");
    require(adm_scene_stream_signal_end(stream, 1, samples.size()) == ADM_ERROR_OK, "signal room end");
    std::uint64_t pulled_frames = 0;
    bool ended = false;
    bool signal = false;
    std::array<float, 514> output{};
    for (int attempt = 0; attempt < 5000 && !ended; ++attempt) {
        adm_scene_pull_result_t result{};
        result.struct_size = sizeof(result);
        require(adm_scene_stream_pull(stream, output.data(), 257, &result) == ADM_ERROR_OK, "pull room");
        require((result.flags & ADM_SCENE_PULL_FAILED) == 0, "room worker failed");
        pulled_frames += result.media_frames;
        signal = signal || std::ranges::any_of(output, [](float v) { return std::abs(v) > 1e-6F; });
        ended = (result.flags & ADM_SCENE_PULL_EOS) != 0;
        if (result.media_frames == 0 && !ended) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto expected = ((samples.size() + tail) * output_rate + 47999) / 48000;
    require(ended && signal && pulled_frames == expected, "Scene appends full tail and rounds output length once");
    require(adm_scene_stream_room_status_json(stream, &report) == ADM_ERROR_OK, "room status");
    auto status = nlohmann::json::parse(report);
    adm_free_string(report);
    require(status.at("material_frames") == samples.size(), "room material/tail counters separate");
    require(status.at("quality") == expected_quality, "quality survives epoch and room requests");
    require(status.at("reflection_update_frames") == inspection.at("reflection_update_frames"),
            "status reports the active reflection policy");
    require(adm_scene_stream_room_source_json(stream, 7, &report) == ADM_ERROR_OK, "actual room source position");
    auto source = nlohmann::json::parse(report);
    adm_free_string(report);
    require(std::abs(source.at("distance_metres").get<double>() - 3.0) < 1e-6, "3 metre Scene source preserved");
    if (screen) {
        require(std::abs(source.at("direction")[0].get<double>() - source.at("direction")[1].get<double>()) < 1e-6,
                "Scene screen edge maps to 45 degrees without changing distance");
    }
    adm_destroy_scene_stream(stream);
    adm_destroy_context(context);
    std::cout << "Scene/C API/resource ownership/EOS passed at " << output_rate << " Hz\n";
}
// An explicit prepared HRIR bank must still authenticate an accompanying SOFA set.
void conflict_test(const std::filesystem::path& root, const std::filesystem::path& sofa) {
    const Room room{root / "room.json"};
    const auto stored = utf8(root / "stored-hrir.json");
    const auto sofa_name = utf8(sofa);
    adm_room_config_t room_config{};
    room_config.struct_size = sizeof(room_config);
    room_config.room = room.room;
    room_config.metres_per_unit = 1.5;
    room_config.hrir_manifest = stored.c_str();
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.rendering.struct_size = sizeof(config.rendering);
    config.rendering.renderer = ADM_RENDERER_ROOM_BINAURAL;
    config.rendering.output_layout = "binaural";
    config.rendering.sofa_path = sofa_name.c_str();
    config.input_sample_rate = 48000;
    config.output_sample_rate = 48000;
    config.output_ring_frames = 32768;
    config.startup_watermark_frames = 1;
    adm_scene_stream_t* stream = nullptr;
    require(adm_create_scene_stream_with_room_ex(room.context, &config, &room_config, nullptr, &stream) !=
                    ADM_ERROR_OK &&
                stream == nullptr,
            "explicit prepared bank must still authenticate an accompanying SOFA");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "fixture directory and SOFA required");
        const std::filesystem::path root{argv[1]};
        {
            auto* context = adm_create_context();
            adm_room_t* bad = nullptr;
            const auto path = utf8(root / "bad.json");
            require(adm_room_load(context, path.c_str(), &bad) != ADM_ERROR_OK && bad == nullptr,
                    "corrupt hash rejected");
            adm_destroy_context(context);
        }
        option_test();
        quality_test(root);
        scene_test(root, 48000);
        scene_test(root, 44100);
        scene_test(root, 48000, true);
        scene_test(root, 48000, false, {}, ADM_ROOM_QUALITY_REALTIME);
        scene_test(root, 44100, true, {}, ADM_ROOM_QUALITY_REALTIME);
        const std::filesystem::path sofa{argv[2]};
        const auto collection = sofa.parent_path() / "distance" / "distance_index.json";
        const Room room{root / "room.json"};
        const auto collection_info = room.inspect(utf8(collection), 1.5, ADM_ROOM_QUALITY_EXACT);
        require(collection_info.at("hrtf_collection").at("radii_m").size() == 2,
                "distance collection inspection reports every layer");
        scene_test(root, 48000, false, collection);
        scene_test(root, 44100, true, collection, ADM_ROOM_QUALITY_REALTIME);
        std::filesystem::rename(root / "hrir.json", root / "stored-hrir.json");
        scene_test(root, 48000, false, sofa);
        scene_test(root, 48000, false, sofa, ADM_ROOM_QUALITY_REALTIME);
        scene_test(root, 44100, false, sofa.parent_path() / "room-delayed.sofa");
        require(room.inspect(utf8(sofa), 1.5, ADM_ROOM_QUALITY_EXACT).at("hrir_directions") == 6,
                "direct SOFA inspection uses native preparation");
        conflict_test(root, sofa);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
