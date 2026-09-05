#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/c_api.h"

#include "../../src/adm_realtime/buffered_media_timeline.h"
#include "../../src/adm_render_common/render_common.h"

namespace {
bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool lfe_layout(const char* layout, uint32_t channels, bool split) {
    auto* context = adm_create_context();
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.rendering.struct_size = sizeof(config.rendering);
    config.rendering.renderer = ADM_RENDERER_SAF;
    config.rendering.output_layout = layout;
    config.rendering.speaker_geometry = ADM_SPEAKER_GEOMETRY_APPLE;
    config.rendering.lfe_routing_mode = split ? ADM_LFE_ROUTING_SPLIT_POWER : ADM_LFE_ROUTING_DIRECT;
    config.input_sample_rate = 48000;
    config.output_sample_rate = 48000;
    config.startup_watermark_frames = 1;
    adm_scene_stream_t* stream = nullptr;
    bool ok = check(adm_create_scene_stream(context, &config, &stream) == ADM_ERROR_OK, "LFE stream creation failed");
    ok &= check(adm_scene_stream_begin_epoch(stream, 1, 0) == ADM_ERROR_OK, "LFE epoch failed");
    adm_scene_element_descriptor_t element{};
    element.struct_size = sizeof(element);
    element.element_id = 9;
    element.role = ADM_SCENE_ELEMENT_LFE;
    element.speaker_label = "LFE1";
    ok &=
        check(adm_scene_stream_configure_generation(stream, 1, 1, &element, 1) == ADM_ERROR_OK, "LFE configure failed");
    adm_scene_initial_state_t initial{};
    initial.struct_size = sizeof(initial);
    initial.element_id = 9;
    initial.state.struct_size = sizeof(initial.state);
    initial.state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN;
    initial.state.active = 1;
    initial.state.linear_gain = 1.0F;
    std::array<float, 1024> samples{};
    samples.fill(1.0F);
    adm_scene_pcm_plane_t plane{};
    plane.struct_size = sizeof(plane);
    plane.element_id = 9;
    plane.samples = samples.data();
    plane.sample_count = 1024;
    plane.stride = 1;
    plane.has_signal = 1;
    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 1;
    frame.generation_id = 1;
    frame.duration_samples = 1024;
    frame.pcm_count = 1;
    frame.pcm = &plane;
    frame.initial_state_count = 1;
    frame.initial_states = &initial;
    int32_t accepted = -1;
    ok &= check(adm_scene_stream_submit_frame(stream, &frame, 100, &accepted) == ADM_ERROR_OK && accepted == 0,
                "LFE submit failed");
    ok &= check(adm_scene_stream_signal_end(stream, 1, 1024) == ADM_ERROR_OK, "LFE end failed");
    std::vector<float> pcm(static_cast<std::size_t>(channels) * 1024U);
    adm_scene_pull_result_t result{};
    result.struct_size = sizeof(result);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        adm_scene_stream_pull(stream, pcm.data(), 1024, &result);
        if (result.media_frames != 0U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= check(result.media_frames == 1024, "LFE render did not produce its frames");
    // Observe steady routing after the existing 10 ms generation declick.
    for (std::size_t sample = 512; sample < 1024U; ++sample) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const float lfe_gain = split ? std::sqrt(0.5F) : 1.0F;
            const float expected = (channel == 3U || (split && channel == 9U)) ? lfe_gain : 0.0F;
            ok &=
                check(std::abs(pcm[(sample * channels) + channel] - expected) < 1.0e-6F, "Incorrect LFE channel/gain");
        }
        if (split) {
            ok &= check(std::abs(mradm::render_common::mix_lfe_pair(
                                     pcm[(sample * channels) + 3U], pcm[(sample * channels) + 9U], true) -
                                 1.0F) < 1.0e-6F,
                        "Windows dual-LFE recombination changed unity gain");
        }
    }
    adm_destroy_scene_stream(stream);
    adm_destroy_context(context);
    return ok;
}

// NOLINTNEXTLINE(readability-function-size): end-to-end ownership and drain scenario.
bool run(bool system_spatial) {
    mradm::realtime::BufferedMediaTimeline timeline;
    timeline.enqueue(0, 33);
    timeline.enqueue(1024, 0);
    if (!check(timeline.advance(500) == 33, "padding advanced the media cursor")) {
        return false;
    }
    timeline.enqueue(2048, 20);
    if (!check(timeline.advance(2000) == 33 && timeline.advance(2058) == 43 && timeline.advance(9000) == 53,
               "PTS gap mapping failed")) {
        return false;
    }
    timeline.reset();
    if (!check(timeline.advance(9000) == 0, "seek retained stale media")) {
        return false;
    }
    auto* context = adm_create_context();
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.rendering.struct_size = sizeof(config.rendering);
    config.rendering.renderer = ADM_RENDERER_SAF;
    config.rendering.output_layout = "4+7+0";
    config.rendering.speaker_geometry = ADM_SPEAKER_GEOMETRY_APPLE;
    config.input_sample_rate = 48000;
    config.output_sample_rate = 48000;
    config.startup_watermark_frames = 1;
    adm_scene_stream_t* stream = nullptr;
    if (!check(adm_create_scene_stream(context, &config, &stream) == ADM_ERROR_OK, "stream creation failed")) {
        adm_destroy_context(context);
        return false;
    }
    adm_scene_output_config_t device{};
    device.struct_size = sizeof(device);
    device.kind = system_spatial ? ADM_SCENE_OUTPUT_SYSTEM_SPATIAL : ADM_SCENE_OUTPUT_NULL;
    device.output_layout = "4+7+0";
    device.speaker_geometry = ADM_SPEAKER_GEOMETRY_APPLE;
    adm_scene_output_t* output = nullptr;
    bool ok =
        check(adm_create_scene_output(context, stream, &device, &output) == ADM_ERROR_OK, "output creation failed");
    adm_scene_output_t* duplicate = nullptr;
    ok &= check(adm_create_scene_output(context, stream, &device, &duplicate) == ADM_ERROR_INVALID_ARGUMENT,
                "a second consumer was allowed");
    adm_scene_pull_result_t pulled{};
    pulled.struct_size = sizeof(pulled);
    ok &= check(adm_scene_stream_pull(stream, nullptr, 0, &pulled) == ADM_ERROR_INVALID_ARGUMENT,
                "direct pull bypassed output ownership");
    ok &= check(adm_scene_stream_begin_epoch(stream, 1, 0) == ADM_ERROR_INVALID_ARGUMENT,
                "direct reset bypassed the device barrier");
    ok &= check(adm_scene_output_begin_epoch(output, 1, 0) == ADM_ERROR_OK, "output reset failed");
    adm_scene_element_descriptor_t element{};
    element.struct_size = sizeof(element);
    element.role = ADM_SCENE_ELEMENT_LFE;
    element.element_id = 7;
    ok &= check(adm_scene_stream_configure_generation(stream, 1, 0, &element, 1) == ADM_ERROR_OK, "configure failed");
    adm_scene_initial_state_t state{};
    state.struct_size = sizeof(state);
    state.element_id = 7;
    state.state.struct_size = sizeof(state.state);
    state.state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN;
    state.state.active = 1;
    state.state.linear_gain = 1;
    std::array<float, 33> samples{};
    samples.fill(0.1F);
    adm_scene_pcm_plane_t plane{};
    plane.struct_size = sizeof(plane);
    plane.element_id = 7;
    plane.samples = samples.data();
    plane.sample_count = samples.size();
    plane.stride = 1;
    plane.has_signal = 1;
    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 1;
    frame.duration_samples = samples.size();
    frame.pcm_count = 1;
    frame.pcm = &plane;
    frame.initial_state_count = 1;
    frame.initial_states = &state;
    int32_t accepted = -1;
    ok &= check(adm_scene_stream_submit_frame(stream, &frame, 100, &accepted) == ADM_ERROR_OK &&
                    accepted == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit failed");
    ok &= check(adm_scene_stream_signal_end(stream, 1, 33) == ADM_ERROR_OK, "end failed");
    ok &= check(adm_scene_output_set_volume(output, 0.0F) == ADM_ERROR_OK, "mute failed");
    ok &= check(adm_scene_output_play(output) == ADM_ERROR_OK, "play failed");
    adm_scene_output_status_t status{};
    status.struct_size = sizeof(status);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        adm_scene_output_get_status(output, &status);
        if (status.ended != 0 || status.failed != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ok &= check(status.ended != 0 && status.presented_frames == 33 && status.consumed_frames == 33,
                "short EOS did not drain at its exact length");
    ok &= check(adm_scene_output_begin_epoch(output, 2, 100) == ADM_ERROR_OK, "seek failed");
    adm_scene_output_get_status(output, &status);
    ok &= check(status.presented_frames == 0 && status.epoch_id == 2 && status.state == ADM_SCENE_OUTPUT_PAUSED,
                "seek leaked old progress");
    ok &= check(adm_scene_output_set_volume(output, -1.0F) == ADM_ERROR_INVALID_ARGUMENT, "invalid volume accepted");
    adm_destroy_scene_stream(stream);
    adm_destroy_scene_output(output);
    adm_destroy_context(context);
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    const bool system_spatial = argc == 2 && std::string_view(argv[1]) == "--system";
    const bool lfe_ok =
        lfe_layout("4+7+0", 12, false) && lfe_layout("9.1.6", 16, false) && lfe_layout("9+10+3", 24, true);
    return lfe_ok && run(system_spatial) ? 0 : 1;
}
