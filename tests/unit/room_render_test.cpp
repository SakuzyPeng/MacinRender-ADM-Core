#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/c_api.h"
#include "adm/render_room.h"

#include "live_room_renderer.h"

namespace {
constexpr const char* k_screen_json = R"({"schema":"mradm.screen-config","version":1,
    "reproduction":{"coordinates":"polar","centre":[0,0,1],"width":90,"aspect_ratio":1.78}})";
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename T> T take(mradm::Result<T> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
    return std::move(*value);
}
void ok(mradm::Result<void> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
}
std::string utf8(const std::filesystem::path& path) {
    auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::vector<float> read(const std::filesystem::path& path) {
    auto reader = take(mradm::audio::FloatWavReader::open(utf8(path)));
    std::vector<float> output(reader.frame_count() * reader.channels());
    require(reader.read(output.data(), reader.frame_count()) == reader.frame_count(), "read rendered fixture");
    return output;
}
void screen_config_test() {
    auto config = take(mradm::parse_screen_config_json(k_screen_json));
    require(config.reproduction && config.reproduction->width == 90, "screen JSON geometry");
    require(!mradm::parse_screen_config_json("{}"), "missing screen schema rejected");
    auto invalid = config;
    invalid.reproduction = mradm::ScreenGeometry{.width = 0.0};
    require(!mradm::validate_screen_config(invalid), "invalid screen width rejected");
    mradm::AdmScene scene;
    mradm::SceneObject child;
    child.id = "child";
    mradm::SceneObject parent;
    parent.id = "parent";
    parent.adm_source = mradm::SceneObjectSource{};
    parent.adm_source->child_objects = {child.id};
    scene.objects = {parent, child};
    mradm::SceneContent content;
    content.id = "content";
    content.object_ids = {parent.id};
    scene.contents = {content};
    mradm::SceneProgramme programme;
    programme.id = "first";
    programme.content_ids = {content.id};
    programme.has_reference_screen = true;
    programme.reference_screen = mradm::ScreenGeometry{};
    programme.reference_screen->width = 60;
    scene.programmes = {programme};
    require(take(mradm::resolve_reference_screen(scene, child.id, config)).width == 60,
            "descendant inherits programme reference screen");
    programme.id = "second";
    programme.reference_screen->width = 70;
    scene.programmes.push_back(programme);
    require(!mradm::resolve_reference_screen(scene, child.id, config), "conflicting programme screens rejected");
    config.reference = mradm::ScreenGeometry{};
    require(take(mradm::resolve_reference_screen(scene, child.id, config)).width == 58,
            "explicit reference overrides programme conflict");
    require(take(mradm::resolve_reference_screen(scene, child.id, {})).width == 58,
            "disabled screen mapping preserves old programme handling");
    auto* options = adm_create_render_options();
    require(adm_render_options_set_screen_config_json(options, k_screen_json) == ADM_ERROR_OK, "C screen setter");
    require(adm_render_options_set_screen_config_json(options, "{}") == ADM_ERROR_INVALID_ARGUMENT,
            "C screen setter rejects malformed input");
    require(adm_render_options_set_screen_config_json(options, nullptr) == ADM_ERROR_OK, "C screen clear");
    adm_destroy_render_options(options);
}

void screened_file_test(mradm::RenderPlan plan, const std::vector<float>& reference) {
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    plan.screens = take(mradm::parse_screen_config_json(k_screen_json));
    plan.render_window.reset();
    for (auto& block : plan.scene.objects[0].tracks[0].blocks) {
        block.screen_ref = true;
    }
    auto renderer = mradm::create_room_renderer();
    auto prepared = take(renderer->prepare(plan, logs));
    take(renderer->render_window(*prepared, plan, progress, logs));
    auto expected = read(plan.output_path);
    require(expected != reference, "screen mapping must affect off-centre file source");
    auto stream = take(renderer->open_stream(*prepared, plan, logs));
    std::array<float, 698> output{};
    std::vector<float> actual;
    for (;;) {
        const auto count = take(stream->process(output, 349));
        if (count == 0) {
            break;
        }
        actual.insert(actual.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(count * 2));
    }
    require(actual == expected, "screen mapping is identical in offline and file streaming");
    ok(stream->seek(20111));
    require(take(stream->process(output, 349)) == 349, "screen seek produces requested frames");
    require(std::equal(output.begin(), output.end(), expected.begin() + 40222), "screen seek retains authored ramp");
}
// NOLINTNEXTLINE(readability-function-size): one integration lifecycle and its assertions.
void file_test(const std::filesystem::path& root, const mradm::RoomConfig& config) {
    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    mradm::RenderPlan plan;
    plan.input_path = utf8(root / "input.wav");
    plan.output_path = utf8(root / "output.wav");
    plan.output_layout = "binaural";
    plan.room = config;
    plan.scene.info.sample_rate = 48000;
    plan.scene.info.num_channels = 1;
    plan.scene.info.num_frames = 29011;
    mradm::SceneObject object;
    object.id = "moving";
    mradm::SceneTrackRef track;
    track.channel_index = 0;
    mradm::SceneObjectBlock first;
    first.position.cartesian = true;
    first.position.y = 1;
    first.end_sample = 17913;
    auto second = first;
    second.start_sample = first.end_sample;
    second.end_sample = plan.scene.info.num_frames;
    second.position.x = -0.8F;
    second.position.y = 1.3F;
    second.interp_length_samples = 3317;
    track.blocks = {first, second};
    object.tracks.push_back(track);
    plan.scene.objects.push_back(object);
    std::vector<float> input(plan.scene.info.num_frames);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(static_cast<int>(i % 89) - 44) / 4096;
    }
    auto writer = take(mradm::audio::FloatWavWriter::open(plan.input_path, 1, 48000));
    require(writer.write(input.data(), input.size()) == input.size(), "write input");
    ok(writer.finish());
    auto renderer = mradm::create_room_renderer();
    auto prepared = take(renderer->prepare(plan, logs));
    take(renderer->render_window(*prepared, plan, progress, logs));
    auto reference = read(plan.output_path);
    auto stream = take(renderer->open_stream(*prepared, plan, logs));
    require(reference.size() / 2 == input.size() + stream->appended_tail_frames(), "offline appends complete tail");
    std::vector<float> actual;
    constexpr std::array<std::size_t, 6> chunks{1, 173, 4097, 255, 19, 2048};
    std::array<float, 8194> out{};
    for (std::size_t i = 0;; ++i) {
        const auto frames = take(stream->process(out, chunks.at(i % chunks.size())));
        if (frames == 0) {
            break;
        }
        actual.insert(actual.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(frames * 2));
    }
    require(reference == actual, "file/offline fragmented PCM must be bit identical");
    for (const std::uint64_t start : std::array<std::uint64_t, 3>{20111, 28333, input.size() + 371U}) {
        ok(stream->seek(start));
        auto count = take(stream->process(out, 701));
        require(count == 701, "seek length");
        require(std::equal(out.begin(), out.begin() + 1402, reference.begin() + static_cast<std::ptrdiff_t>(start * 2)),
                "finite seek reproduces uninterrupted render");
    }
    plan.render_window = mradm::RenderWindow{20111, 877};
    plan.output_path = utf8(root / "window.wav");
    take(renderer->render_window(*prepared, plan, progress, logs));
    const auto window = read(plan.output_path);
    require(window.size() == 1754 && std::equal(window.begin(), window.end(), reference.begin() + 40222),
            "exact offline window");
    auto edited = take(renderer->open_stream(*prepared, plan, logs));
    mradm::LiveOverrides overrides;
    mradm::LiveObjectOverride edit;
    edit.object_id = "moving";
    edit.mute = true;
    edit.head_locked = true;
    overrides.objects.push_back(edit);
    ok(edited->validate_overrides(overrides));
    edited->set_overrides(overrides);
    require(take(edited->process(out, 2048)) == 2048 &&
                std::all_of(out.begin(), out.begin() + 4096, [](float x) { return x == 0; }),
            "initial live mute is immediate");
    ok(edited->seek(17011));
    require(take(edited->process(out, 701)) == 701 &&
                std::all_of(out.begin(), out.begin() + 1402, [](float x) { return x == 0; }),
            "live controls survive seek");
    overrides.objects[0].extent_scale = -1;
    require(!edited->validate_overrides(overrides), "invalid live control rejected");
    plan.output_path = utf8(root / "screened.wav");
    screened_file_test(plan, reference);
    std::cout << "file/offline/FIFO/seek/window/tail passed\n";
}

void superseded_request_test(mradm::IRoomControl& control,
                             const mradm::RoomConfig& config,
                             const mradm::RoomConfig& other,
                             const mradm::RoomConfig& incompatible,
                             const auto& tick,
                             const auto& wait) {
    const auto retained = take(control.request(config));
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::transitioning; });
    take(control.request(other));
    // Pump preparation without advancing the transition: this must enter the
    // kernel's pending slot, rather than starting another audible transition.
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::transitioning; }, false);
    const auto failed_latest = take(control.request(incompatible));
    wait(
        [&](const auto& s) {
            return s.state == mradm::RoomSwitchState::failed && s.requested_revision == failed_latest;
        },
        false);
    wait([&](const auto& s) { return s.applied_revision == retained; });
    for (int i = 0; i < 100; ++i) {
        tick();
    }
    require(control.status().applied_revision == retained,
            "failed newest request must not revive a superseded pending room");
    const auto restored = take(control.request(other));
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::applied && s.applied_revision == restored; });
}

void switching_test(const std::filesystem::path& root, const mradm::RoomConfig& config) {
    namespace live = mradm::live_scene;
    live::RendererConfig rendering;
    rendering.renderer = mradm::RendererSelection::room_binaural;
    rendering.output_layout = "binaural";
    rendering.room = config;
    auto renderer = take(live::create_live_room_renderer(rendering, {}));
    const std::array elements{live::ElementDescriptor{.element_id = 7, .has_position = true}};
    ok(renderer->configure_generation(1, elements));
    auto control = renderer->room_control();
    live::Frame frame;
    frame.generation_id = 1;
    frame.duration_samples = 256;
    frame.pcm.push_back({7, true, std::vector<float>(256, 0.01F)});
    live::ObjectState initial;
    initial.valid_fields = live::k_known_state_fields;
    frame.initial_states.push_back({7, initial});
    std::array<float, 512> output{};
    ok(renderer->render(frame, output));
    frame.initial_states.clear();
    auto tick = [&](bool advance = true) {
        frame.duration_samples = advance ? 256U : 0U;
        frame.media_sample_start += frame.duration_samples;
        ok(renderer->render(frame, output));
        require(std::ranges::all_of(output, [](float v) { return std::isfinite(v); }), "switch finite output");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    auto wait = [&](const auto& predicate, bool advance = true) {
        for (int i = 0; i < 2000; ++i) {
            tick(advance);
            if (predicate(control->status())) {
                return;
            }
        }
        throw std::runtime_error("room switch did not reach the expected state");
    };
    const mradm::RoomConfig other{take(mradm::RoomProfile::load(root / "second.json")), {}, 1.5};
    const mradm::RoomConfig incompatible{take(mradm::RoomProfile::load(root / "incompatible.json")), {}, 1.5};
    const auto bad = take(control->request(incompatible));
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::failed && s.requested_revision == bad; });
    require(control->status().applied_revision == 0, "failed candidate retains current room");
    take(control->request(other));
    const auto latest = take(control->request(config));
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::applied && s.applied_revision == latest; });
    const auto change = take(control->request(other));
    wait([&](const auto& s) {
        return s.state == mradm::RoomSwitchState::transitioning && s.requested_revision == change;
    });
    const auto sample = control->status().transition_sample;
    take(control->request(config));
    const auto final = take(control->request(other));
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::applied && s.applied_revision == final; });
    require(control->status().transition_sample == sample, "same incoming room does not restart transition");
    const auto same = take(control->request(other));
    wait([&](const auto& s) { return s.state == mradm::RoomSwitchState::applied && s.applied_revision == same; });
    require(control->status().transition_sample == sample, "same current room does not restart transition");
    superseded_request_test(*control, config, other, incompatible, tick, wait);
    (*renderer).reset();
    ok(renderer->configure_generation(2, elements));
    rendering.room = other;
    auto fresh = take(live::create_live_room_renderer(rendering, {}));
    ok(fresh->configure_generation(2, elements));
    frame.generation_id = 2;
    frame.media_sample_start = 0;
    frame.initial_states.push_back({7, initial});
    std::array<float, 512> expected{};
    for (int i = 0; i < 20; ++i) {
        ok(renderer->render(frame, output));
        ok(fresh->render(frame, expected));
        require(output == expected, "epoch reset preserves applied room and clears history");
        frame.initial_states.clear();
        frame.media_sample_start += 256;
    }
    std::cout << "background/failure/latest/same-room/reset passed\n";
}

void preparation_capacity_test(const std::filesystem::path& root, const mradm::RoomConfig& config) {
    namespace live = mradm::live_scene;
    live::RendererConfig rendering;
    rendering.renderer = mradm::RendererSelection::room_binaural;
    rendering.output_layout = "binaural";
    rendering.room = config;
    auto renderer = take(live::create_live_room_renderer(rendering, {}));
    auto control = renderer->room_control();
    const mradm::RoomConfig other{take(mradm::RoomProfile::load(root / "second.json")), {}, 1.5};
    std::vector<live::ElementDescriptor> elements;
    live::Frame frame;
    frame.duration_samples = 256;
    std::array<float, 512> output{};
    // Cover both the zero-source session before its first frame and a later
    // generation that adds sources while a room is being prepared.
    for (std::uint64_t generation = 1; generation <= 2; ++generation) {
        const auto request = take(control->request(generation == 1 ? other : config));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        live::ElementDescriptor description;
        description.element_id = generation;
        description.has_position = true;
        elements.push_back(description);
        ok(renderer->configure_generation(generation, elements));
        frame.generation_id = generation;
        frame.pcm.push_back({generation, true, std::vector<float>(256, 0.01F)});
        for (int attempt = 0; attempt < 2000 && control->status().applied_revision != request; ++attempt) {
            ok(renderer->render(frame, output));
            frame.media_sample_start += 256;
            require(control->status().state != mradm::RoomSwitchState::failed,
                    "source growth must resize the candidate without failing the room switch");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(control->status().applied_revision == request, "resized room candidate must eventually apply");
    }
    std::cout << "preparation/source capacity growth passed\n";
}

// NOLINTNEXTLINE(readability-function-size): one integration lifecycle and its assertions.
void scene_test(const std::filesystem::path& root, std::uint32_t output_rate, bool screen = false) {
    auto* context = adm_create_context();
    require(context != nullptr, "context");
    adm_room_t* room = nullptr;
    const auto manifest = utf8(root / "room.json");
    require(adm_room_load(context, manifest.c_str(), &room) == ADM_ERROR_OK, "C room load");
    adm_room_config_t room_config{};
    room_config.struct_size = sizeof(room_config);
    room_config.room = room;
    room_config.metres_per_unit = 1.5;
    char* report = nullptr;
    require(adm_room_inspect(context, &room_config, &report) == ADM_ERROR_OK, "C room inspect");
    const auto inspection = nlohmann::json::parse(report);
    adm_free_string(report);
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
    adm_scene_stream_t* stream = nullptr;
    require(adm_create_scene_stream_with_room_ex(
                context, &config, &room_config, screen ? k_screen_json : nullptr, &stream) == ADM_ERROR_OK,
            "create Scene room stream");
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
    plane.sample_count = samples.size();
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
    frame.duration_samples = samples.size();
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
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "fixture directory required");
        const std::filesystem::path root{argv[1]};
        auto room = take(mradm::RoomProfile::load(root / "room.json"));
        require(!mradm::RoomProfile::load(root / "bad.json"), "corrupt hash rejected");
        screen_config_test();
        file_test(root, {room, {}, 1.5});
        switching_test(root, {room, {}, 1.5});
        preparation_capacity_test(root, {room, {}, 1.5});
        scene_test(root, 48000);
        scene_test(root, 44100);
        scene_test(root, 48000, true);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
