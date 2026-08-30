#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "adm/c_api.h"

namespace {

static_assert(std::is_standard_layout_v<adm_scene_stream_config_t>);
static_assert(std::is_standard_layout_v<adm_scene_renderer_config_t>);
static_assert(std::is_standard_layout_v<adm_scene_semantic_entity_t>);
static_assert(std::is_standard_layout_v<adm_scene_semantic_identity_t>);
static_assert(std::is_standard_layout_v<adm_scene_element_descriptor_t>);
static_assert(std::is_standard_layout_v<adm_scene_object_state_t>);
static_assert(std::is_standard_layout_v<adm_scene_pcm_plane_t>);
static_assert(std::is_standard_layout_v<adm_scene_initial_state_t>);
static_assert(std::is_standard_layout_v<adm_scene_metadata_update_t>);
static_assert(std::is_standard_layout_v<adm_scene_frame_t>);
static_assert(std::is_standard_layout_v<adm_scene_output_format_t>);
static_assert(std::is_standard_layout_v<adm_scene_pull_result_t>);
static_assert(std::is_standard_layout_v<adm_scene_stream_status_t>);
static_assert(std::is_standard_layout_v<adm_scene_diagnostic_t>);
static_assert(offsetof(adm_scene_stream_config_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_renderer_config_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_semantic_entity_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_semantic_identity_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_element_descriptor_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_object_state_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_pcm_plane_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_initial_state_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_metadata_update_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_frame_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_output_format_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_pull_result_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_stream_status_t, struct_size) == 0U);
static_assert(offsetof(adm_scene_diagnostic_t, struct_size) == 0U);

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

adm_scene_stream_config_t stream_config(std::uint32_t input_rate = 48000U, std::uint32_t output_rate = 48000U) {
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.rendering.struct_size = sizeof(config.rendering);
    config.rendering.renderer = ADM_RENDERER_SAF;
    config.rendering.output_layout = "0+2+0";
    config.rendering.speaker_geometry = ADM_SPEAKER_GEOMETRY_STANDARD;
    config.rendering.speaker_spread_mode = ADM_SPEAKER_SPREAD_AUTOMATIC;
    config.rendering.binaural_spread_mode = ADM_BINAURAL_SPREAD_AUTOMATIC;
    config.rendering.lfe_routing_mode = ADM_LFE_ROUTING_DIRECT;
    config.input_sample_rate = input_rate;
    config.output_sample_rate = output_rate;
    config.input_queue_samples = 4096U;
    config.input_queue_bytes = 1024ULL * 1024ULL;
    config.output_ring_frames = 2048U;
    config.startup_watermark_frames = 1U;
    return config;
}

adm_scene_renderer_config_t vbap_renderer_config(std::string_view layout = "0+2+0") {
    adm_scene_renderer_config_t config{};
    config.struct_size = sizeof(config);
    config.renderer = ADM_RENDERER_SAF;
    config.output_layout = layout.data();
    config.speaker_geometry = ADM_SPEAKER_GEOMETRY_STANDARD;
    config.speaker_spread_mode = ADM_SPEAKER_SPREAD_AUTOMATIC;
    config.binaural_spread_mode = ADM_BINAURAL_SPREAD_AUTOMATIC;
    config.lfe_routing_mode = ADM_LFE_ROUTING_DIRECT;
    return config;
}

adm_scene_renderer_config_t binaural_renderer_config(const char* sofa_path = nullptr) {
    auto config = vbap_renderer_config("binaural");
    config.renderer = ADM_RENDERER_SAF_BINAURAL;
    config.sofa_path = sofa_path;
    return config;
}

adm_scene_object_state_t complete_state(float gain = 1.0F) {
    adm_scene_object_state_t state{};
    state.struct_size = sizeof(state);
    state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN | ADM_SCENE_STATE_POSITION |
                         ADM_SCENE_STATE_EXTENT | ADM_SCENE_STATE_DIFFUSE | ADM_SCENE_STATE_DIVERGENCE |
                         ADM_SCENE_STATE_CHANNEL_LOCK | ADM_SCENE_STATE_SCREEN_REFERENCE | ADM_SCENE_STATE_HEAD_LOCKED |
                         ADM_SCENE_STATE_DIVERGENCE_RANGE | ADM_SCENE_STATE_CHANNEL_LOCK_MAX_DISTANCE;
    state.active = 1;
    state.linear_gain = gain;
    state.position_y = 1.0F;
    state.divergence_azimuth_range = 45.0F;
    return state;
}

struct StreamGuard {
    StreamGuard() = default;
    StreamGuard(const StreamGuard&) = delete;
    StreamGuard& operator=(const StreamGuard&) = delete;
    StreamGuard(StreamGuard&&) = delete;
    StreamGuard& operator=(StreamGuard&&) = delete;

    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    adm_scene_stream_t* value{nullptr};
    ~StreamGuard() { adm_destroy_scene_stream(value); }
};

bool create_stream(adm_context_t* context, const adm_scene_stream_config_t& config, StreamGuard& stream) {
    return check(adm_create_scene_stream(context, &config, &stream.value) == ADM_ERROR_OK,
                 "create live Scene stream") &&
           check(stream.value != nullptr, "create returned a stream handle");
}

bool configure_object(adm_scene_stream_t* stream, std::uint64_t epoch, std::uint64_t generation) {
    struct ForwardDescriptor {
        adm_scene_element_descriptor_t descriptor;
        // cppcheck-suppress unusedStructMember
        std::uint64_t future_field;
    } descriptor{};
    descriptor.descriptor.struct_size = sizeof(descriptor);
    descriptor.descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
    descriptor.descriptor.element_id = 7U;
    descriptor.descriptor.has_position = 1;
    descriptor.descriptor.position_y = 1.0F;
    return check(adm_scene_stream_configure_generation(stream, epoch, generation, &descriptor.descriptor, 1U) ==
                     ADM_ERROR_OK,
                 "configure object generation");
}

adm_scene_frame_t object_frame(std::uint64_t epoch,
                               std::uint64_t generation,
                               std::int64_t start,
                               const std::vector<float>& samples,
                               adm_scene_pcm_plane_t& plane,
                               adm_scene_initial_state_t& initial) {
    plane = {};
    plane.struct_size = sizeof(plane);
    plane.element_id = 7U;
    plane.samples = samples.data();
    plane.sample_count = static_cast<std::uint32_t>(samples.size());
    plane.stride = 1U;
    plane.has_signal = 1;

    initial = {};
    initial.struct_size = sizeof(initial);
    initial.element_id = 7U;
    initial.state = complete_state();

    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = epoch;
    frame.generation_id = generation;
    frame.media_sample_start = start;
    frame.duration_samples = static_cast<std::uint32_t>(samples.size());
    frame.pcm_count = 1U;
    frame.pcm = &plane;
    frame.initial_state_count = 1U;
    frame.initial_states = &initial;
    return frame;
}

bool wait_for_output(adm_scene_stream_t* stream,
                     std::uint32_t channels,
                     std::uint64_t expected_frames,
                     bool& saw_signal,
                     std::vector<float>* captured = nullptr) {
    std::uint64_t media_frames = 0U;
    bool saw_eos = false;
    for (int attempt = 0; attempt < 5000 && !saw_eos; ++attempt) {
        std::vector<float> output(static_cast<std::size_t>(64U) * channels, -99.0F);
        adm_scene_pull_result_t pulled{};
        pulled.struct_size = sizeof(pulled);
        if (!check(adm_scene_stream_pull(stream, output.data(), 64U, &pulled) == ADM_ERROR_OK,
                   "pull live Scene output")) {
            return false;
        }
        media_frames += pulled.media_frames;
        const auto media_samples = static_cast<std::size_t>(pulled.media_frames) * channels;
        if (captured != nullptr) {
            captured->insert(
                captured->end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(media_samples));
        }
        saw_signal = saw_signal || std::any_of(output.begin(),
                                               output.begin() + static_cast<std::ptrdiff_t>(media_samples),
                                               [](float sample) { return std::fabs(sample) > 1.0e-7F; });
        const auto padding = output.begin() + static_cast<std::ptrdiff_t>(media_samples);
        if (!check(std::all_of(padding, output.end(), [](float sample) { return sample == 0.0F; }),
                   "pull zero-fills every non-media sample")) {
            return false;
        }
        saw_eos = (pulled.flags & ADM_SCENE_PULL_EOS) != 0U;
        if (!saw_eos && pulled.media_frames == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return check(saw_eos, "live Scene stream reaches EOS") &&
           check(media_frames == expected_frames, "live Scene output length matches the rational timeline");
}

bool pull_open_stream(adm_scene_stream_t* stream,
                      std::uint32_t channels,
                      std::uint32_t expected_frames,
                      std::vector<float>& captured) {
    std::uint32_t media_frames = 0U;
    for (int attempt = 0; attempt < 5000 && media_frames < expected_frames; ++attempt) {
        const auto requested = std::min<std::uint32_t>(64U, expected_frames - media_frames);
        std::vector<float> output(static_cast<std::size_t>(requested) * channels, -99.0F);
        adm_scene_pull_result_t pulled{};
        pulled.struct_size = sizeof(pulled);
        if (!check(adm_scene_stream_pull(stream, output.data(), requested, &pulled) == ADM_ERROR_OK,
                   "pull open live Scene stream") ||
            !check((pulled.flags & ADM_SCENE_PULL_FAILED) == 0U, "open live Scene stream remains healthy")) {
            return false;
        }
        captured.insert(captured.end(),
                        output.begin(),
                        output.begin() +
                            (static_cast<std::ptrdiff_t>(pulled.media_frames) * static_cast<std::ptrdiff_t>(channels)));
        media_frames += pulled.media_frames;
        if (pulled.media_frames == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return check(media_frames == expected_frames, "open live Scene pull reaches the requested media frame");
}

bool wait_for_policy_revision(adm_scene_stream_t* stream, std::uint64_t revision) {
    for (int attempt = 0; attempt < 5000; ++attempt) {
        adm_scene_stream_status_t status{};
        status.struct_size = sizeof(status);
        if (adm_scene_stream_get_status(stream, &status) == ADM_ERROR_OK &&
            status.semantic_policy_revision == revision) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return check(false, "live Scene worker applies semantic policy revision");
}

float mean_absolute_tail(const std::vector<float>& samples, std::size_t channels, std::size_t tail_frames) {
    if (samples.empty() || channels == 0U) {
        return 0.0F;
    }
    const auto total_frames = samples.size() / channels;
    const auto first_frame = total_frames > tail_frames ? total_frames - tail_frames : 0U;
    double sum = 0.0;
    std::size_t count = 0U;
    for (std::size_t frame = first_frame; frame < total_frames; ++frame) {
        for (std::size_t channel = 0U; channel < channels; ++channel) {
            sum += std::fabs(samples[(frame * channels) + channel]);
            ++count;
        }
    }
    return count == 0U ? 0.0F : static_cast<float>(sum / static_cast<double>(count));
}

float mean_absolute_channel_tail(const std::vector<float>& samples,
                                 std::size_t channels,
                                 std::size_t channel,
                                 std::size_t tail_frames) {
    if (samples.empty() || channels == 0U || channel >= channels) {
        return 0.0F;
    }
    const auto total_frames = samples.size() / channels;
    const auto first_frame = total_frames > tail_frames ? total_frames - tail_frames : 0U;
    double sum = 0.0;
    for (std::size_t frame = first_frame; frame < total_frames; ++frame) {
        sum += std::fabs(samples[(frame * channels) + channel]);
    }
    const auto count = total_frames - first_frame;
    return count == 0U ? 0.0F : static_cast<float>(sum / static_cast<double>(count));
}

bool test_deep_copy_and_timeline(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    if (!create_stream(context, config, stream)) {
        return false;
    }
    adm_scene_output_format_t format{};
    format.struct_size = sizeof(format);
    bool ok = check(adm_scene_stream_get_output_format(stream.value, &format) == ADM_ERROR_OK,
                    "query live Scene output format") &&
              check(format.sample_format == ADM_SCENE_SAMPLE_F32 && format.sample_rate == 48000U &&
                        format.channels == 2U && format.interleaved != 0,
                    "live Scene output is interleaved normalized f32 stereo");
    adm_scene_output_format_t prefix_format{};
    prefix_format.struct_size = offsetof(adm_scene_output_format_t, channels);
    prefix_format.channels = 0xA5A5A5A5U;
    ok &= check(adm_scene_stream_get_output_format(stream.value, &prefix_format) == ADM_ERROR_OK &&
                    prefix_format.sample_format == ADM_SCENE_SAMPLE_F32 && prefix_format.sample_rate == 48000U &&
                    prefix_format.channels == 0xA5A5A5A5U,
                "smaller output POD prefix is written without touching a future tail");
    ok &= check(adm_scene_stream_begin_epoch(stream.value, 1U, 100) == ADM_ERROR_OK, "begin first Scene epoch");
    ok &= configure_object(stream.value, 1U, 11U);

    std::vector<float> samples(128U, 0.25F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(1U, 11U, 100, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit complete Scene frame");
    std::ranges::fill(samples, 0.0F); // caller memory may be overwritten immediately after submit
    plane.samples = nullptr;
    ok &= check(adm_scene_stream_signal_end(stream.value, 1U, 228) == ADM_ERROR_OK, "signal exact Scene EOS");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 128U, saw_signal);
    ok &= check(saw_signal, "deep-copied PCM survives caller overwrite");

    ok &= check(adm_scene_stream_begin_epoch(stream.value, 2U, 1000) == ADM_ERROR_OK,
                "new epoch resets old input/output/DSP state");
    ok &= configure_object(stream.value, 2U, 12U);
    samples.assign(16U, 0.1F);
    frame = object_frame(2U, 12U, 1001, samples, plane, initial);
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "first frame may not start after epoch target");
    frame.media_sample_start = 1000;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK,
                "continuous frame at epoch target is accepted");
    frame.media_sample_start = 1017;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "timeline gap is rejected without dropping the accepted frame");
    ok &= check(adm_scene_stream_begin_epoch(stream.value, 2U, 0) == ADM_ERROR_INVALID_ARGUMENT,
                "epoch IDs must increase");
    return ok;
}

bool test_validation_and_backpressure(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.input_queue_samples = 64U;
    config.output_ring_frames = 1U;
    config.startup_watermark_frames = 1U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 10U, 0) == ADM_ERROR_OK, "begin queue test epoch");
    ok &= configure_object(stream.value, 10U, 1U);

    std::vector<float> samples(64U, 0.2F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(10U, 1U, 0, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "queue accepts first frame");
    frame.media_sample_start = 64;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_WOULD_BLOCK,
                "full bounded queue returns would-block without dropping");
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 3U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_TIMED_OUT,
                "finite queue wait returns timed-out without dropping");

    std::vector<float> oversized_samples(65U, 0.2F);
    adm_scene_pcm_plane_t oversized_plane{};
    adm_scene_initial_state_t oversized_initial{};
    auto oversized = object_frame(10U, 1U, 64, oversized_samples, oversized_plane, oversized_initial);
    ok &= check(adm_scene_stream_submit_frame(stream.value, &oversized, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "one frame larger than the sample budget is rejected immediately");
    ok &= check(adm_scene_stream_signal_end(stream.value, 10U, 64) == ADM_ERROR_OK,
                "queue test closes at the next accepted input sample");
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_CLOSED,
                "submit after EOS reports closed");

    ok &= check(adm_scene_stream_begin_epoch(stream.value, 11U, 0) == ADM_ERROR_OK,
                "epoch reset interrupts a worker blocked on the output ring");
    ok &= configure_object(stream.value, 11U, 1U);
    samples.assign(4U, 0.1F);
    samples[2] = std::numeric_limits<float>::quiet_NaN();
    frame = object_frame(11U, 1U, 0, samples, plane, initial);
    auto bad_pointer = frame;
    bad_pointer.pcm = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &bad_pointer, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "non-zero PCM count rejects a null array pointer");
    auto unknown_generation = frame;
    unknown_generation.generation_id = 999U;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &unknown_generation, 0U, &submit) ==
                    ADM_ERROR_INVALID_ARGUMENT,
                "unknown generation is rejected");
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "non-finite PCM is rejected");
    samples[2] = 0.1F;
    plane.sample_count = 3U;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "wrong PCM sample count is rejected");
    // cppcheck-suppress redundantAssignment
    plane.sample_count = 4U;
    initial.state.linear_gain = std::numeric_limits<float>::infinity();
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "non-finite canonical state is rejected");
    // cppcheck-suppress redundantAssignment
    initial.state.linear_gain = -0.1F;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "negative canonical linear gain is rejected");
    // cppcheck-suppress redundantAssignment
    initial.state.linear_gain = 1.0F;
    initial.state.position_x = 1.1F;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "out-of-range canonical Cartesian position is rejected");
    // cppcheck-suppress redundantAssignment
    initial.state.position_x = 0.0F;
    plane.has_signal = 0;
    plane.samples = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "has_signal=false accepts a null PCM pointer as exact-duration silence");
    ok &= check(adm_scene_stream_signal_end(stream.value, 11U, 4) == ADM_ERROR_OK,
                "silent optimized frame closes on its exact timeline");
    bool saw_signal = false;
    std::vector<float> silent_output;
    ok &= wait_for_output(stream.value, 2U, 4U, saw_signal, &silent_output);
    ok &= check(!saw_signal && std::ranges::all_of(silent_output, [](float sample) { return sample == 0.0F; }),
                "has_signal=false produces only media silence");

    StreamGuard byte_limited;
    auto byte_config = stream_config();
    byte_config.input_queue_bytes = 64U;
    ok &= create_stream(context, byte_config, byte_limited);
    ok &= check(adm_scene_stream_begin_epoch(byte_limited.value, 12U, 0) == ADM_ERROR_OK, "begin byte-budget epoch");
    ok &= configure_object(byte_limited.value, 12U, 1U);
    samples.assign(4U, 0.1F);
    frame = object_frame(12U, 1U, 0, samples, plane, initial);
    ok &= check(adm_scene_stream_submit_frame(byte_limited.value, &frame, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "one frame larger than the owned-byte budget is rejected immediately");
    return ok;
}

bool test_resampling_length(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config(48000U, 44100U);
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 20U, 0) == ADM_ERROR_OK, "begin resampling epoch") &&
              configure_object(stream.value, 20U, 1U);
    std::vector<float> samples(480U, 0.1F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(20U, 1U, 0, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit frame for 48 to 44.1 kHz conversion");
    ok &= check(adm_scene_stream_signal_end(stream.value, 20U, 480) == ADM_ERROR_OK, "signal resampled Scene EOS");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 441U, saw_signal);
    ok &= check(saw_signal, "resampled live Scene carries signal");

    StreamGuard high_rate_stream;
    auto high_rate_config = stream_config(48000U, 96000U);
    if (!create_stream(context, high_rate_config, high_rate_stream)) {
        return false;
    }
    ok &= check(adm_scene_stream_begin_epoch(high_rate_stream.value, 21U, 0) == ADM_ERROR_OK,
                "begin 96 kHz resampling epoch");
    ok &= configure_object(high_rate_stream.value, 21U, 1U);
    samples.assign(480U, 0.1F);
    frame = object_frame(21U, 1U, 0, samples, plane, initial);
    ok &= check(adm_scene_stream_submit_frame(high_rate_stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit frame for 48 to 96 kHz conversion");
    ok &= check(adm_scene_stream_signal_end(high_rate_stream.value, 21U, 480) == ADM_ERROR_OK,
                "signal 96 kHz resampled Scene EOS");
    saw_signal = false;
    ok &= wait_for_output(high_rate_stream.value, 2U, 960U, saw_signal);
    ok &= check(saw_signal, "96 kHz resampled live Scene carries signal");
    return ok;
}

bool test_resampling_preroll_boundary(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config(48000U, 44100U);
    config.output_ring_frames = 512U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 32U, 0) == ADM_ERROR_OK, "begin resampled preroll epoch") &&
        configure_object(stream.value, 32U, 1U);
    int32_t submit = -1;

    std::vector<float> first_samples(256U, -1.0F);
    adm_scene_pcm_plane_t first_plane{};
    adm_scene_initial_state_t first_initial{};
    auto first = object_frame(32U, 1U, -512, first_samples, first_plane, first_initial);
    first.flags |= ADM_SCENE_FRAME_DISCONTINUITY;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &first, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit a fully hidden resampled SceneFrame");

    std::vector<float> crossing_samples(416U, -1.0F);
    std::fill(crossing_samples.begin() + 256, crossing_samples.end(), 1.0F);
    adm_scene_pcm_plane_t crossing_plane{};
    adm_scene_initial_state_t crossing_initial{};
    auto crossing = object_frame(32U, 1U, -256, crossing_samples, crossing_plane, crossing_initial);
    crossing.initial_state_count = 0U;
    crossing.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &crossing, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit a resampled SceneFrame crossing the target boundary");
    ok &= check(adm_scene_stream_signal_end(stream.value, 32U, 160) == ADM_ERROR_OK,
                "close the resampled preroll timeline");

    bool saw_signal = false;
    std::vector<float> output;
    ok &= wait_for_output(stream.value, 2U, 147U, saw_signal, &output);
    ok &= check(saw_signal && output.size() == 294U, "resampled preroll preserves the exact audible length");
    if (output.size() >= 32U) {
        const float leading_mean = std::accumulate(output.begin(), output.begin() + 32, 0.0F) / 32.0F;
        ok &= check(leading_mean > 0.0F, "resampler latency cannot leak pre-target PCM into the audible output prefix");
    }
    return ok;
}

bool test_rational_accumulator_many_frames(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config(48000U, 44100U);
    config.input_queue_samples = 65536U;
    config.input_queue_bytes = 4ULL * 1024ULL * 1024ULL;
    config.output_ring_frames = 50000U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 22U, 0) == ADM_ERROR_OK,
                    "begin long rational-accumulator epoch") &&
              configure_object(stream.value, 22U, 1U);
    constexpr std::int64_t k_total_input = 48001;
    constexpr std::uint32_t k_chunk = 127U;
    std::int64_t start = 0;
    int32_t submit = -1;
    while (ok && start < k_total_input) {
        const auto duration = static_cast<std::uint32_t>(std::min<std::int64_t>(k_chunk, k_total_input - start));
        std::vector<float> samples(duration, 0.05F);
        adm_scene_pcm_plane_t plane{};
        adm_scene_initial_state_t initial{};
        auto frame = object_frame(22U, 1U, start, samples, plane, initial);
        if (start != 0) {
            frame.initial_state_count = 0U;
            frame.initial_states = nullptr;
        }
        ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 1000U, &submit) == ADM_ERROR_OK &&
                        submit == ADM_SCENE_SUBMIT_ACCEPTED,
                    "submit one irregularly-sized long-run Scene frame");
        start += duration;
    }
    ok &= check(adm_scene_stream_signal_end(stream.value, 22U, k_total_input) == ADM_ERROR_OK,
                "long rational timeline closes exactly");
    constexpr std::uint64_t k_expected_output = 44101U;
    bool saw_signal = false;
    std::vector<float> output;
    ok &= wait_for_output(stream.value, 2U, k_expected_output, saw_signal, &output);
    ok &= check(saw_signal && output.size() == k_expected_output * 2U,
                "many SceneFrames retain one continuous rational output timeline");
    ok &= check(std::ranges::all_of(output, [](float sample) { return std::isfinite(sample); }),
                "long resampled output stays finite");

    adm_scene_stream_status_t status{};
    status.struct_size = sizeof(status);
    ok &= check(adm_scene_stream_get_status(stream.value, &status) == ADM_ERROR_OK &&
                    status.media_frames_pulled == k_expected_output && status.ended != 0 && status.failed == 0,
                "status reports the completed long rational timeline");
    return ok;
}

bool test_generation_topology_contract(adm_context_t* context) {
    StreamGuard stream;
    const auto config = stream_config();
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 23U, 0) == ADM_ERROR_OK, "begin generation-topology epoch");
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
    descriptor.element_id = 7U;
    descriptor.has_position = 1;
    descriptor.position_y = 1.0F;
    ok &= check(adm_scene_stream_configure_generation(stream.value, 23U, 5U, &descriptor, 1U) == ADM_ERROR_OK,
                "configure a generation topology");
    ok &= check(adm_scene_stream_configure_generation(stream.value, 23U, 5U, &descriptor, 1U) == ADM_ERROR_OK,
                "identical repeated generation configuration is idempotent");
    descriptor.position_x = 0.25F;
    ok &= check(adm_scene_stream_configure_generation(stream.value, 23U, 5U, &descriptor, 1U) ==
                    ADM_ERROR_INVALID_ARGUMENT,
                "configured generation topology is immutable");
    descriptor.position_x = 0.0F;
    std::array<adm_scene_element_descriptor_t, 2U> duplicates{descriptor, descriptor};
    ok &= check(adm_scene_stream_configure_generation(stream.value, 23U, 6U, duplicates.data(), 2U) ==
                    ADM_ERROR_INVALID_ARGUMENT,
                "duplicate element IDs are rejected");
    struct ExtendedDescriptor {
        adm_scene_element_descriptor_t descriptor;
        std::uint64_t future_field;
    };
    std::array<ExtendedDescriptor, 2U> inconsistent{};
    inconsistent[0].descriptor = descriptor;
    inconsistent[0].descriptor.struct_size = sizeof(ExtendedDescriptor);
    inconsistent[0].descriptor.element_id = 8U;
    inconsistent[1].descriptor = descriptor;
    inconsistent[1].descriptor.struct_size = sizeof(adm_scene_element_descriptor_t);
    inconsistent[1].descriptor.element_id = 9U;
    ok &= check(
        adm_scene_stream_configure_generation(
            stream.value, 23U, 7U, &inconsistent[0].descriptor, static_cast<std::uint32_t>(inconsistent.size())) ==
            ADM_ERROR_INVALID_ARGUMENT,
        "POD array elements must all use the first element's struct_size stride");
    ok &= check(adm_scene_stream_configure_generation(stream.value, 999U, 6U, &descriptor, 1U) ==
                    ADM_ERROR_INVALID_ARGUMENT,
                "generation configuration rejects an unknown epoch");
    return ok;
}

bool test_zero_generation_id(adm_context_t* context) {
    StreamGuard stream;
    const auto config = stream_config();
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 33U, 0) == ADM_ERROR_OK, "begin generation-zero epoch") &&
        configure_object(stream.value, 33U, 0U);
    std::vector<float> samples(64U, 0.25F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(33U, 0U, 0, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit SceneFrame using generation zero");
    ok &= check(adm_scene_stream_signal_end(stream.value, 33U, 64) == ADM_ERROR_OK, "close generation-zero epoch");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 64U, saw_signal);
    ok &= check(saw_signal, "generation zero configures its renderer and produces PCM");
    return ok;
}

bool test_sample_accurate_ramp(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 1024U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 24U, 0) == ADM_ERROR_OK, "begin sample-accurate ramp epoch") &&
        configure_object(stream.value, 24U, 1U);
    int32_t submit = -1;

    std::vector<float> preroll_samples(512U, 1.0F);
    adm_scene_pcm_plane_t preroll_plane{};
    adm_scene_initial_state_t preroll_initial{};
    auto preroll = object_frame(24U, 1U, -512, preroll_samples, preroll_plane, preroll_initial);
    preroll.flags |= ADM_SCENE_FRAME_DISCONTINUITY;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &preroll, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "first frame accepts an explicit discontinuity after epoch reset");

    std::vector<float> first_samples(4U, 1.0F);
    adm_scene_pcm_plane_t first_plane{};
    adm_scene_initial_state_t first_initial{};
    auto first = object_frame(24U, 1U, 0, first_samples, first_plane, first_initial);
    first.initial_state_count = 0U;
    first.initial_states = nullptr;
    adm_scene_metadata_update_t fade{};
    fade.struct_size = sizeof(fade);
    fade.element_id = 7U;
    fade.offset_samples = 2U;
    fade.ramp_duration_samples = 4U;
    fade.changed_fields = ADM_SCENE_STATE_LINEAR_GAIN;
    fade.state = complete_state(0.0F);
    first.metadata_update_count = 1U;
    first.metadata_updates = &fade;
    first.flags |= ADM_SCENE_FRAME_DISCONTINUITY;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &first, 0U, &submit) == ADM_ERROR_INVALID_ARGUMENT,
                "discontinuity flag on a later frame requires a new epoch");
    first.flags &= ~ADM_SCENE_FRAME_DISCONTINUITY;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &first, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "gain ramp starts at an exact input-sample offset");

    std::vector<float> second_samples(4U, 1.0F);
    adm_scene_pcm_plane_t second_plane{};
    adm_scene_initial_state_t second_initial{};
    auto second = object_frame(24U, 1U, 4, second_samples, second_plane, second_initial);
    second.initial_state_count = 0U;
    second.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &second, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "gain ramp continues into the next SceneFrame");

    std::vector<float> ordered_samples(1U, 1.0F);
    adm_scene_pcm_plane_t ordered_plane{};
    adm_scene_initial_state_t ordered_initial{};
    auto ordered = object_frame(24U, 1U, 8, ordered_samples, ordered_plane, ordered_initial);
    ordered.initial_state_count = 0U;
    ordered.initial_states = nullptr;
    std::array<adm_scene_metadata_update_t, 2U> updates{};
    for (auto& update : updates) {
        update.struct_size = sizeof(update);
        update.element_id = 7U;
        update.changed_fields = ADM_SCENE_STATE_LINEAR_GAIN;
        update.state = complete_state();
    }
    updates[0].state.linear_gain = 0.25F;
    updates[1].state.linear_gain = 0.5F;
    ordered.metadata_update_count = static_cast<std::uint32_t>(updates.size());
    ordered.metadata_updates = updates.data();
    ok &= check(adm_scene_stream_submit_frame(stream.value, &ordered, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "same-offset events are accepted in caller array order");
    ok &= check(adm_scene_stream_signal_end(stream.value, 24U, 9) == ADM_ERROR_OK,
                "sample-accurate ramp timeline closes exactly");

    bool saw_signal = false;
    std::vector<float> output;
    ok &= wait_for_output(stream.value, 2U, 9U, saw_signal, &output);
    ok &= check(saw_signal && output.size() == 18U, "sample-accurate ramp produces nine stereo media frames");
    if (output.size() == 18U) {
        const auto channel = [&](std::size_t frame) { return output.at(frame * 2U); };
        const float base = channel(0U);
        constexpr float k_tolerance = 2.0e-4F;
        const auto close = [&](float actual, float expected) { return std::fabs(actual - expected) <= k_tolerance; };
        ok &= check(base > 0.0F && close(channel(1U), base) && close(channel(2U), base),
                    "ramp leaves samples before and at its event offset unchanged");
        ok &= check(close(channel(3U), base * 0.75F) && close(channel(4U), base * 0.5F) &&
                        close(channel(5U), base * 0.25F) && close(channel(6U), 0.0F) && close(channel(7U), 0.0F),
                    "four-sample gain ramp is continuous across the SceneFrame boundary");
        ok &= check(close(channel(8U), base * 0.5F),
                    "same-offset updates execute in array order and the final target wins");
    }
    return ok;
}

bool test_incomplete_generation_silence(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 1024U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 26U, 0) == ADM_ERROR_OK,
                    "begin incomplete-state generation epoch") &&
              configure_object(stream.value, 26U, 1U);
    int32_t submit = -1;
    std::vector<float> preroll_samples(512U, 1.0F);
    adm_scene_pcm_plane_t preroll_plane{};
    adm_scene_initial_state_t preroll_initial{};
    auto preroll = object_frame(26U, 1U, -512, preroll_samples, preroll_plane, preroll_initial);
    ok &= check(adm_scene_stream_submit_frame(stream.value, &preroll, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "preroll establishes non-zero old-generation DSP output");
    ok &= configure_object(stream.value, 26U, 2U);

    std::vector<float> incomplete_samples(16U, 1.0F);
    adm_scene_pcm_plane_t incomplete_plane{};
    adm_scene_initial_state_t incomplete_initial{};
    auto incomplete = object_frame(26U, 2U, 0, incomplete_samples, incomplete_plane, incomplete_initial);
    incomplete.flags = 0U;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &incomplete, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "state-incomplete frame is accepted for DSP recovery");

    std::vector<float> warmup_samples(16U, 1.0F);
    adm_scene_pcm_plane_t warmup_plane{};
    adm_scene_initial_state_t warmup_initial{};
    auto warmup = object_frame(26U, 2U, 16, warmup_samples, warmup_plane, warmup_initial);
    warmup.initial_state_count = 0U;
    warmup.initial_states = nullptr;
    warmup.flags = ADM_SCENE_FRAME_STATE_COMPLETE | ADM_SCENE_FRAME_WARMUP;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &warmup, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "warmup frame is accepted for DSP recovery");
    ok &= check(adm_scene_stream_signal_end(stream.value, 26U, 32) == ADM_ERROR_OK,
                "incomplete/warmup timeline closes exactly");
    bool saw_signal = false;
    std::vector<float> output;
    ok &= wait_for_output(stream.value, 2U, 32U, saw_signal, &output);
    ok &= check(!saw_signal && std::ranges::all_of(output, [](float sample) { return sample == 0.0F; }),
                "incomplete and warmup frames stay inaudible even across generation de-click");
    return ok;
}

bool test_pull_state_contract(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 128U;
    config.startup_watermark_frames = 32U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 27U, 0) == ADM_ERROR_OK, "begin pull-state epoch") &&
              configure_object(stream.value, 27U, 1U);

    std::vector<float> initial_silence(32U, -1.0F);
    adm_scene_pull_result_t pulled{};
    pulled.struct_size = sizeof(pulled);
    ok &= check(adm_scene_stream_pull(stream.value, initial_silence.data(), 16U, &pulled) == ADM_ERROR_OK &&
                    pulled.media_frames == 0U && pulled.first_media_frame == 0U &&
                    (pulled.flags & ADM_SCENE_PULL_BUFFERING) != 0U &&
                    std::ranges::all_of(initial_silence, [](float sample) { return sample == 0.0F; }),
                "buffering pull is non-blocking, zero-filled, and does not advance media");

    std::vector<float> samples(64U, 0.2F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(27U, 1U, 0, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit finite media for underrun pull test");
    bool ready = false;
    for (int attempt = 0; attempt < 1000 && !ready; ++attempt) {
        adm_scene_stream_status_t status{};
        status.struct_size = sizeof(status);
        if (adm_scene_stream_get_status(stream.value, &status) == ADM_ERROR_OK &&
            status.buffered_output_frames >= 64U) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= check(ready, "worker reaches the configured startup watermark");

    std::vector<float> output(256U, -1.0F);
    pulled = {};
    pulled.struct_size = sizeof(pulled);
    ok &= check(adm_scene_stream_pull(stream.value, output.data(), 128U, &pulled) == ADM_ERROR_OK &&
                    pulled.media_frames == 64U && pulled.first_media_frame == 0U &&
                    (pulled.flags & ADM_SCENE_PULL_UNDERRUN) != 0U,
                "running pull returns available media first and marks the short tail as underrun");
    const auto media_end = output.begin() + static_cast<std::ptrdiff_t>(64U * 2U);
    ok &= check(std::any_of(output.begin(), media_end, [](float sample) { return sample != 0.0F; }) &&
                    std::all_of(media_end, output.end(), [](float sample) { return sample == 0.0F; }),
                "underrun pull keeps valid media at the front and zero-fills only the shortage");

    ok &= check(adm_scene_stream_signal_end(stream.value, 27U, 64) == ADM_ERROR_OK,
                "pull-state epoch closes at the next expected sample");
    bool saw_eos = false;
    for (int attempt = 0; attempt < 1000 && !saw_eos; ++attempt) {
        std::array<float, 32U> tail{};
        pulled = {};
        pulled.struct_size = sizeof(pulled);
        ok &= check(adm_scene_stream_pull(stream.value, tail.data(), 16U, &pulled) == ADM_ERROR_OK,
                    "pull after EOS remains non-blocking");
        saw_eos = (pulled.flags & ADM_SCENE_PULL_EOS) != 0U;
        if (!saw_eos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ok &= check(saw_eos && pulled.first_media_frame == 64U && pulled.media_frames == 0U,
                "EOS pull reports the unchanged final media position");
    adm_scene_stream_status_t status{};
    status.struct_size = sizeof(status);
    ok &= check(adm_scene_stream_get_status(stream.value, &status) == ADM_ERROR_OK && status.ended != 0 &&
                    status.failed == 0 && status.media_frames_pulled == 64U && status.underruns >= 1U,
                "status exposes ended, media-position, and underrun counters");
    return ok;
}

bool test_worker_failure_pull(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 64U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 28U, 0) == ADM_ERROR_OK, "begin worker-failure epoch");
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_DIRECT_SPEAKER;
    descriptor.element_id = 42U;
    descriptor.speaker_label = "UNKNOWN-LIVE-SPEAKER";
    ok &= check(adm_scene_stream_configure_generation(stream.value, 28U, 1U, &descriptor, 1U) == ADM_ERROR_OK,
                "configure a legal DirectSpeakers role requiring backend routing resolution");

    std::vector<float> samples(8U, 0.2F);
    adm_scene_pcm_plane_t plane{};
    plane.struct_size = sizeof(plane);
    plane.element_id = 42U;
    plane.samples = samples.data();
    plane.sample_count = static_cast<std::uint32_t>(samples.size());
    plane.stride = 1U;
    plane.has_signal = 1;
    adm_scene_initial_state_t initial{};
    initial.struct_size = sizeof(initial);
    initial.element_id = 42U;
    initial.state.struct_size = sizeof(initial.state);
    initial.state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN;
    initial.state.active = 1;
    initial.state.linear_gain = 1.0F;
    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 28U;
    frame.generation_id = 1U;
    frame.duration_samples = static_cast<std::uint32_t>(samples.size());
    frame.pcm_count = 1U;
    frame.pcm = &plane;
    frame.initial_state_count = 1U;
    frame.initial_states = &initial;
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "backend-resolved frame is accepted into the owned queue");

    bool failed = false;
    for (int attempt = 0; attempt < 1000 && !failed; ++attempt) {
        adm_scene_stream_status_t status{};
        status.struct_size = sizeof(status);
        if (adm_scene_stream_get_status(stream.value, &status) == ADM_ERROR_OK && status.failed != 0) {
            failed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= check(failed, "unrenderable backend-native DirectSpeakers semantics fail asynchronously");
    std::array<float, 32U> output{};
    std::ranges::fill(output, -1.0F);
    adm_scene_pull_result_t pulled{};
    pulled.struct_size = sizeof(pulled);
    ok &= check(adm_scene_stream_pull(stream.value, output.data(), 16U, &pulled) == ADM_ERROR_OK &&
                    pulled.media_frames == 0U && (pulled.flags & ADM_SCENE_PULL_FAILED) != 0U &&
                    std::ranges::all_of(output, [](float sample) { return sample == 0.0F; }),
                "worker failure pull remains zero-filled and non-blocking");
    bool saw_backend_failure = false;
    const auto count = adm_scene_stream_log_count(stream.value);
    for (std::uint32_t index = 0U; index < count; ++index) {
        adm_scene_diagnostic_t diagnostic{};
        diagnostic.struct_size = sizeof(diagnostic);
        if (adm_scene_stream_log_entry(stream.value, index, &diagnostic) != 0 &&
            diagnostic.code == ADM_SCENE_DIAGNOSTIC_BACKEND_FAILURE && diagnostic.epoch_id == 28U) {
            saw_backend_failure = true;
        }
    }
    ok &= check(saw_backend_failure, "worker failure is exposed as a structured backend diagnostic");
    return ok;
}

// This end-to-end fixture intentionally keeps the complete three-role Scene transaction together.
// NOLINTNEXTLINE(readability-function-size)
bool test_renderer_native_roles_and_events(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.rendering.output_layout = "0+5+0";
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 25U, 0) == ADM_ERROR_OK, "begin native-role Scene epoch");

    std::vector<adm_scene_element_descriptor_t> descriptors(3U);
    for (auto& descriptor : descriptors) {
        descriptor.struct_size = sizeof(descriptor);
    }
    descriptors[0].role = ADM_SCENE_ELEMENT_OBJECT;
    descriptors[0].element_id = 1U;
    descriptors[0].has_position = 1;
    descriptors[0].position_y = 1.0F;
    descriptors[1].role = ADM_SCENE_ELEMENT_DIRECT_SPEAKER;
    descriptors[1].element_id = 2U;
    descriptors[1].speaker_label = "M+030";
    descriptors[2].role = ADM_SCENE_ELEMENT_LFE;
    descriptors[2].element_id = 3U;
    descriptors[2].speaker_label = "LFE1";
    ok &= check(adm_scene_stream_configure_generation(stream.value, 25U, 9U, descriptors.data(), 3U) == ADM_ERROR_OK,
                "configure object, DirectSpeakers, and LFE topology");

    std::vector<float> object_pcm(128U, 0.05F);
    std::vector<float> direct_pcm(128U, 0.03F);
    std::vector<float> lfe_pcm(128U, 0.02F);
    std::vector<adm_scene_pcm_plane_t> planes(3U);
    const std::array<std::uint64_t, 3U> ids{1U, 2U, 3U};
    const std::array<const float*, 3U> data{object_pcm.data(), direct_pcm.data(), lfe_pcm.data()};
    for (std::size_t index = 0U; index < planes.size(); ++index) {
        planes.at(index).struct_size = sizeof(planes.at(index));
        planes.at(index).element_id = ids.at(index);
        planes.at(index).samples = data.at(index);
        planes.at(index).sample_count = 128U;
        planes.at(index).stride = 1U;
        planes.at(index).has_signal = 1;
    }
    std::vector<adm_scene_initial_state_t> states(3U);
    for (std::size_t index = 0U; index < states.size(); ++index) {
        states.at(index).struct_size = sizeof(states.at(index));
        states.at(index).element_id = ids.at(index);
        states.at(index).state = complete_state();
    }
    states[0].state.extent_width = 0.25F;
    states[0].state.diffuse = 0.2F;

    adm_scene_metadata_update_t update{};
    update.struct_size = sizeof(update);
    update.element_id = 1U;
    update.offset_samples = 17U;
    update.ramp_duration_samples = 200U; // intentionally crosses this SceneFrame boundary
    update.changed_fields = ADM_SCENE_STATE_LINEAR_GAIN | ADM_SCENE_STATE_POSITION | ADM_SCENE_STATE_EXTENT |
                            ADM_SCENE_STATE_DIFFUSE | ADM_SCENE_STATE_DIVERGENCE;
    update.state = complete_state(0.5F);
    update.state.position_x = 0.5F;
    update.state.position_y = 1.0F;
    update.state.extent_width = 0.5F;
    update.state.extent_height = 0.25F;
    update.state.diffuse = 0.4F;
    update.state.divergence = 0.3F;

    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 25U;
    frame.generation_id = 9U;
    frame.media_sample_start = 0;
    frame.duration_samples = 128U;
    frame.pcm_count = 3U;
    frame.pcm = planes.data();
    frame.initial_state_count = 3U;
    frame.initial_states = states.data();
    frame.metadata_update_count = 1U;
    frame.metadata_updates = &update;
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit all Renderer-native roles and sample-accurate metadata");
    ok &= check(adm_scene_stream_signal_end(stream.value, 25U, 128) == ADM_ERROR_OK, "signal native-role Scene EOS");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 6U, 128U, saw_signal);
    ok &= check(saw_signal, "object, DirectSpeakers, and LFE produce output");

    const uint32_t diagnostic_count = adm_scene_stream_log_count(stream.value);
    ok &= check(diagnostic_count > 0U, "best-effort semantic degradation is diagnosed");
    if (diagnostic_count > 0U) {
        adm_scene_diagnostic_t diagnostic{};
        diagnostic.struct_size = sizeof(diagnostic);
        ok &= check(adm_scene_stream_log_entry(stream.value, 0U, &diagnostic) != 0 && diagnostic.message != nullptr &&
                        diagnostic.epoch_id == 25U && diagnostic.generation_id == 9U,
                    "structured Scene diagnostic carries timeline identity and message");
    }
    return ok;
}

bool test_binaural_backend(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.rendering.renderer = ADM_RENDERER_SAF_BINAURAL;
    config.rendering.output_layout = "binaural";
    config.output_ring_frames = 1024U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 30U, 0) == ADM_ERROR_OK, "begin binaural Scene epoch") &&
              configure_object(stream.value, 30U, 1U);
    std::vector<float> samples(128U, 0.1F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(30U, 1U, 0, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit frame to SAF binaural Scene backend");
    ok &= check(adm_scene_stream_signal_end(stream.value, 30U, 128) == ADM_ERROR_OK, "signal binaural Scene EOS");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 128U, saw_signal);
    ok &= check(saw_signal, "SAF binaural live Scene produces HRTF-filtered PCM");
    return ok;
}

bool render_binaural_position(adm_context_t* context,
                              std::uint64_t epoch,
                              float position_x,
                              std::vector<float>& output) {
    StreamGuard stream;
    auto config = stream_config();
    config.rendering.renderer = ADM_RENDERER_SAF_BINAURAL;
    config.rendering.output_layout = "binaural";
    config.output_ring_frames = 1024U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, epoch, 0) == ADM_ERROR_OK,
                    "begin positioned binaural Scene epoch") &&
              configure_object(stream.value, epoch, 1U);
    std::vector<float> samples(256U, 0.1F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(epoch, 1U, 0, samples, plane, initial);
    initial.state.position_x = position_x;
    initial.state.position_y = 0.0F;
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit positioned binaural SceneFrame");
    ok &= check(adm_scene_stream_signal_end(stream.value, epoch, 256) == ADM_ERROR_OK,
                "close positioned binaural Scene epoch");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 256U, saw_signal, &output);
    return ok && check(saw_signal, "positioned binaural Scene produces PCM");
}

bool test_binaural_dynamic_position(adm_context_t* context) {
    std::vector<float> left_output;
    std::vector<float> right_output;
    bool ok = render_binaural_position(context, 34U, -1.0F, left_output);
    ok &= render_binaural_position(context, 35U, 1.0F, right_output);
    ok &= check(left_output.size() == right_output.size() && !left_output.empty(),
                "positioned binaural comparisons have matching output lengths");
    if (left_output.size() == right_output.size()) {
        float maximum_difference = 0.0F;
        for (std::size_t index = 0U; index < left_output.size(); ++index) {
            maximum_difference = std::max(maximum_difference, std::fabs(left_output[index] - right_output[index]));
        }
        ok &= check(maximum_difference > 1.0e-6F,
                    "binaural rendering follows current object state instead of the fixed descriptor position");
    }
    return ok;
}

// Backend preparation is synchronous, while takeover is worker-owned. This test keeps
// one generation alive across failed and rapid successful switches so state retention
// and the pre-resampler crossfade are exercised together.
// NOLINTNEXTLINE(readability-function-size)
bool test_backend_hot_switch(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 8192U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 40U, 0) == ADM_ERROR_OK, "begin backend-switch Scene epoch") &&
        configure_object(stream.value, 40U, 1U);
    ok &= check(adm_scene_stream_set_semantic_policy_json(
                    stream.value, R"({"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":1}}})", 71U) ==
                    ADM_ERROR_OK,
                "install neutral policy before backend switch");
    ok &= wait_for_policy_revision(stream.value, 71U);
    ok &= check(adm_scene_stream_set_listener_orientation(stream.value, 12.0F, -4.0F, 3.0F) == ADM_ERROR_OK,
                "set pose retained by backend switch");

    std::vector<float> first_samples(1024U, 0.05F);
    adm_scene_pcm_plane_t first_plane{};
    adm_scene_initial_state_t first_initial{};
    auto first = object_frame(40U, 1U, 0, first_samples, first_plane, first_initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &first, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit pre-switch Scene audio");
    std::vector<float> output;
    ok &= pull_open_stream(stream.value, 2U, 1024U, output);

    auto invalid_sofa = binaural_renderer_config("/definitely/missing/mradm-scene-stream-test.sofa");
    ok &= check(adm_scene_stream_switch_backend(stream.value, &invalid_sofa) != ADM_ERROR_OK,
                "invalid external SOFA rejects synchronously");
    auto vbap_with_sofa = vbap_renderer_config();
    vbap_with_sofa.sofa_path = "/definitely/missing/unused-vbap.sofa";
    ok &= check(adm_scene_stream_switch_backend(stream.value, &vbap_with_sofa) == ADM_ERROR_UNSUPPORTED,
                "SOFA configuration is rejected for non-binaural SAF rendering");
    auto incompatible = vbap_renderer_config("0+5+0");
    ok &= check(adm_scene_stream_switch_backend(stream.value, &incompatible) == ADM_ERROR_UNSUPPORTED,
                "backend switch rejects a channel-count change");
    adm_scene_stream_status_t preserved{};
    preserved.struct_size = sizeof(preserved);
    ok &= check(adm_scene_stream_get_status(stream.value, &preserved) == ADM_ERROR_OK && preserved.failed == 0 &&
                    preserved.generation_id == 1U && preserved.semantic_policy_revision == 71U,
                "failed backend preparation preserves stream, generation, and policy status");

    if (const char* sofa = std::getenv("MR_ADM_TEST_SOFA_PATH"); sofa != nullptr && sofa[0] != '\0') {
        auto external = binaural_renderer_config(sofa);
        ok &= check(adm_scene_stream_switch_backend(stream.value, &external) == ADM_ERROR_OK,
                    "valid external SOFA prepares for Scene hot switch");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto kemar = binaural_renderer_config();
        ok &= check(adm_scene_stream_switch_backend(stream.value, &kemar) == ADM_ERROR_OK,
                    "empty SOFA path restores built-in KEMAR");
    }

    // Only the newest pending renderer matters. Deliberately issue more updates than
    // the worker can be required to consume between calls.
    auto binaural = binaural_renderer_config();
    auto vbap = vbap_renderer_config();
    ok &= check(adm_scene_stream_switch_backend(stream.value, &binaural) == ADM_ERROR_OK,
                "prepare first rapid backend switch");
    ok &= check(adm_scene_stream_switch_backend(stream.value, &vbap) == ADM_ERROR_OK,
                "prepare superseding rapid backend switch");
    ok &= check(adm_scene_stream_switch_backend(stream.value, &binaural) == ADM_ERROR_OK,
                "prepare latest rapid backend switch");

    std::vector<float> second_samples(4096U, 0.05F);
    adm_scene_pcm_plane_t second_plane{};
    adm_scene_initial_state_t second_initial{};
    auto second = object_frame(40U, 1U, 1024, second_samples, second_plane, second_initial);
    second.initial_state_count = 0U;
    second.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &second, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit audio spanning the backend crossfade");
    ok &=
        check(adm_scene_stream_signal_end(stream.value, 40U, 5120) == ADM_ERROR_OK, "close backend-switch Scene epoch");
    bool saw_signal = false;
    std::vector<float> switched_output;
    ok &= wait_for_output(stream.value, 2U, 4096U, saw_signal, &switched_output);
    output.insert(output.end(), switched_output.begin(), switched_output.end());
    ok &= check(saw_signal && std::ranges::all_of(output, [](float sample) { return std::isfinite(sample); }),
                "rapid backend switches keep finite audible output");
    float maximum_step = 0.0F;
    for (std::size_t index = 2U; index < output.size(); ++index) {
        maximum_step = std::max(maximum_step, std::fabs(output[index] - output[index - 2U]));
    }
    ok &= check(maximum_step < 0.25F, "backend takeover has no hard full-scale discontinuity");
    preserved = {};
    preserved.struct_size = sizeof(preserved);
    ok &= check(adm_scene_stream_get_status(stream.value, &preserved) == ADM_ERROR_OK && preserved.failed == 0 &&
                    preserved.generation_id == 1U && preserved.semantic_policy_revision == 71U,
                "successful backend switch retains generation, pose, and policy state");
    return ok;
}

// NOLINTNEXTLINE(readability-function-size)
bool test_semantic_policy_hot_replace(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 4096U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok =
        check(adm_scene_stream_begin_epoch(stream.value, 41U, 0) == ADM_ERROR_OK, "begin semantic-policy Scene epoch");

    adm_scene_semantic_entity_t content{};
    content.struct_size = sizeof(content);
    content.id = "ACO_1";
    content.name = "Dialogue";
    adm_scene_semantic_entity_t programme{};
    programme.struct_size = sizeof(programme);
    programme.id = "APR_1";
    programme.name = "Main";
    adm_scene_semantic_identity_t identity{};
    identity.struct_size = sizeof(identity);
    identity.object_id = "AO_1";
    identity.object_name = "Voice";
    identity.track_uid = "ATU_1";
    identity.has_importance = 1;
    identity.importance = 7;
    identity.has_dialogue_id = 1;
    identity.dialogue_id = 1;
    identity.content_count = 1U;
    identity.contents = &content;
    identity.programme_count = 1U;
    identity.programmes = &programme;
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
    descriptor.element_id = 7U;
    descriptor.has_position = 1;
    descriptor.position_y = 1.0F;
    descriptor.semantic_identity = &identity;
    ok &= check(adm_scene_stream_configure_generation(stream.value, 41U, 1U, &descriptor, 1U) == ADM_ERROR_OK,
                "configure Scene semantic identity");

    constexpr const char* k_mute =
        R"({"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1","gain":{"mute":true}}]})";
    constexpr const char* k_half =
        R"({"schema":"mradm.semantic-policy.v1","objects":[{"programme":"Main","gain":{"scale":0.5}}]})";
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_mute, 1U) == ADM_ERROR_OK,
                "install object-ID semantic policy");
    ok &= wait_for_policy_revision(stream.value, 1U);
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, "{not-json", 99U) == ADM_ERROR_INVALID_ARGUMENT,
                "invalid policy JSON is rejected synchronously");
    adm_scene_stream_status_t status{};
    status.struct_size = sizeof(status);
    ok &= check(adm_scene_stream_get_status(stream.value, &status) == ADM_ERROR_OK &&
                    status.semantic_policy_revision == 1U,
                "invalid policy keeps the previous applied revision");

    int32_t submit = -1;
    std::vector<float> samples(2048U, 0.1F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(41U, 1U, 0, samples, plane, initial);
    frame.duration_samples = 1024U;
    plane.sample_count = 1024U;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit muted semantic-policy block");
    std::vector<float> muted;
    ok &= pull_open_stream(stream.value, 2U, 1024U, muted);
    ok &= check(std::ranges::all_of(muted, [](float sample) { return sample == 0.0F; }),
                "object identity selector mutes the matching Scene element");

    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_half, 2U) == ADM_ERROR_OK,
                "replace semantic policy while running");
    ok &= wait_for_policy_revision(stream.value, 2U);
    frame = object_frame(41U, 1U, 1024, samples, plane, initial);
    frame.initial_state_count = 0U;
    frame.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit policy transition block");
    std::vector<float> half_transition;
    ok &= pull_open_stream(stream.value, 2U, 2048U, half_transition);
    const float half_level = mean_absolute_tail(half_transition, 2U, 256U);
    ok &= check(half_level > 0.0F, "policy replacement fades from mute to the new effective gain");

    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_half, 3U) == ADM_ERROR_OK,
                "replace policy with the same arithmetic target");
    ok &= wait_for_policy_revision(stream.value, 3U);
    samples.assign(1024U, 0.1F);
    frame = object_frame(41U, 1U, 3072, samples, plane, initial);
    frame.initial_state_count = 0U;
    frame.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit repeated-policy block");
    std::vector<float> repeated;
    ok &= pull_open_stream(stream.value, 2U, 1024U, repeated);
    const float repeated_level = mean_absolute_tail(repeated, 2U, 256U);
    ok &= check(half_level > 0.0F && std::fabs(repeated_level - half_level) < half_level * 0.05F,
                "replacing a policy recomputes from producer baseline instead of compounding gain");

    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, nullptr, 4U) == ADM_ERROR_OK,
                "NULL semantic policy clears the active policy");
    ok &= wait_for_policy_revision(stream.value, 4U);
    samples.assign(2048U, 0.1F);
    frame = object_frame(41U, 1U, 4096, samples, plane, initial);
    frame.initial_state_count = 0U;
    frame.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit policy-clear transition block");
    ok &= check(adm_scene_stream_signal_end(stream.value, 41U, 6144) == ADM_ERROR_OK,
                "close semantic-policy Scene epoch");
    bool saw_signal = false;
    std::vector<float> restored;
    ok &= wait_for_output(stream.value, 2U, 2048U, saw_signal, &restored);
    const float restored_level = mean_absolute_tail(restored, 2U, 256U);
    ok &= check(saw_signal && restored_level > half_level * 1.8F && restored_level < half_level * 2.2F,
                "clearing policy restores the unmodified producer baseline with a smooth transition");

    constexpr const char* k_unmatched =
        R"({"schema":"mradm.semantic-policy.v1","objects":[{"id":"missing","gain":{"mute":true}}]})";
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_unmatched, 5U) == ADM_ERROR_OK,
                "accept an unmatched but valid semantic rule");
    ok &= wait_for_policy_revision(stream.value, 5U);
    std::uint32_t unmatched_diagnostics = 0U;
    const auto diagnostic_count = adm_scene_stream_log_count(stream.value);
    for (std::uint32_t index = 0U; index < diagnostic_count; ++index) {
        adm_scene_diagnostic_t diagnostic{};
        diagnostic.struct_size = sizeof(diagnostic);
        if (adm_scene_stream_log_entry(stream.value, index, &diagnostic) != 0 && diagnostic.message != nullptr &&
            std::string_view{diagnostic.message}.find("objects[0] did not match") != std::string_view::npos) {
            ++unmatched_diagnostics;
        }
    }
    ok &= check(unmatched_diagnostics == 1U, "an unmatched Scene semantic rule emits one diagnostic");
    return ok;
}

// Legacy/no-identity input, AudioObject track aggregation, and DirectSpeakers use
// the same policy resolver but take different Scene conversion paths.
// NOLINTNEXTLINE(readability-function-size)
bool test_semantic_identity_grouping_and_roles(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 50U, 0) == ADM_ERROR_OK,
                    "begin grouped semantic identity epoch");
    std::array<adm_scene_semantic_identity_t, 2U> identities{};
    std::array<adm_scene_element_descriptor_t, 2U> descriptors{};
    for (std::size_t index = 0U; index < descriptors.size(); ++index) {
        auto& identity = identities.at(index);
        auto& descriptor = descriptors.at(index);
        identity.struct_size = sizeof(identity);
        identity.object_id = "AO_GROUP";
        identity.object_name = "Grouped object";
        identity.track_uid = index == 0U ? "ATU_A" : "ATU_B";
        descriptor.struct_size = sizeof(descriptor);
        descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
        descriptor.element_id = index + 1U;
        descriptor.has_position = 1;
        descriptor.position_x = index == 0U ? -0.5F : 0.5F;
        descriptor.position_y = 1.0F;
        descriptor.semantic_identity = &identity;
    }
    ok &= check(adm_scene_stream_configure_generation(
                    stream.value, 50U, 1U, descriptors.data(), static_cast<std::uint32_t>(descriptors.size())) ==
                    ADM_ERROR_OK,
                "configure two tracks of one semantic AudioObject");
    constexpr const char* k_track_policy =
        R"({"schema":"mradm.semantic-policy.v1","objects":[{"track_uid":"ATU_B","gain":{"mute":true}}]})";
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_track_policy, 10U) == ADM_ERROR_OK,
                "install grouped track-UID policy");
    ok &= wait_for_policy_revision(stream.value, 10U);
    std::array<std::vector<float>, 2U> samples{std::vector<float>(128U, 0.05F), std::vector<float>(128U, 0.05F)};
    std::array<adm_scene_pcm_plane_t, 2U> planes{};
    std::array<adm_scene_initial_state_t, 2U> initials{};
    for (std::size_t index = 0U; index < planes.size(); ++index) {
        auto& plane = planes.at(index);
        auto& initial = initials.at(index);
        plane.struct_size = sizeof(plane);
        plane.element_id = index + 1U;
        plane.samples = samples.at(index).data();
        plane.sample_count = 128U;
        plane.stride = 1U;
        plane.has_signal = 1;
        initial.struct_size = sizeof(initial);
        initial.element_id = index + 1U;
        initial.state = complete_state();
    }
    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 50U;
    frame.generation_id = 1U;
    frame.duration_samples = 128U;
    frame.pcm_count = static_cast<std::uint32_t>(planes.size());
    frame.pcm = planes.data();
    frame.initial_state_count = static_cast<std::uint32_t>(initials.size());
    frame.initial_states = initials.data();
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit grouped semantic object tracks");
    ok &= check(adm_scene_stream_signal_end(stream.value, 50U, 128) == ADM_ERROR_OK,
                "close grouped semantic identity epoch");
    bool saw_signal = false;
    std::vector<float> output;
    ok &= wait_for_output(stream.value, 2U, 128U, saw_signal, &output);
    ok &= check(!saw_signal && std::ranges::all_of(output, [](float sample) { return sample == 0.0F; }),
                "one track-UID match applies to every element sharing its object_id");

    ok &= check(adm_scene_stream_begin_epoch(stream.value, 51U, 0) == ADM_ERROR_OK,
                "begin legacy no-identity policy epoch");
    ok &= configure_object(stream.value, 51U, 1U);
    constexpr const char* k_legacy_policy =
        R"({"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":1}},"objects":[{"all":true,"gain":{"mute":true}},{"id":"missing","gain":{"mute":true}}]})";
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_legacy_policy, 11U) == ADM_ERROR_OK,
                "install global/all policy for a descriptor without identity");
    ok &= wait_for_policy_revision(stream.value, 11U);
    std::vector<float> legacy_samples(128U, 0.05F);
    adm_scene_pcm_plane_t legacy_plane{};
    adm_scene_initial_state_t legacy_initial{};
    auto legacy_frame = object_frame(51U, 1U, 0, legacy_samples, legacy_plane, legacy_initial);
    ok &= check(adm_scene_stream_submit_frame(stream.value, &legacy_frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit legacy no-identity Scene element");
    ok &= check(adm_scene_stream_signal_end(stream.value, 51U, 128) == ADM_ERROR_OK,
                "close legacy no-identity policy epoch");
    saw_signal = false;
    output.clear();
    ok &= wait_for_output(stream.value, 2U, 128U, saw_signal, &output);
    ok &= check(!saw_signal, "global/all semantic rules remain available without identity");
    std::uint32_t missing_count = 0U;
    for (std::uint32_t index = 0U; index < adm_scene_stream_log_count(stream.value); ++index) {
        adm_scene_diagnostic_t diagnostic{};
        diagnostic.struct_size = sizeof(diagnostic);
        if (adm_scene_stream_log_entry(stream.value, index, &diagnostic) != 0 && diagnostic.message != nullptr &&
            std::string_view{diagnostic.message}.find("objects[1] did not match") != std::string_view::npos) {
            ++missing_count;
        }
    }
    ok &= check(missing_count == 1U, "non-global legacy selector emits one unmatched diagnostic");

    ok &=
        check(adm_scene_stream_begin_epoch(stream.value, 52U, 0) == ADM_ERROR_OK, "begin DirectSpeakers policy epoch");
    adm_scene_semantic_identity_t direct_identity{};
    direct_identity.struct_size = sizeof(direct_identity);
    direct_identity.object_id = "AO_DS";
    adm_scene_element_descriptor_t direct{};
    direct.struct_size = sizeof(direct);
    direct.role = ADM_SCENE_ELEMENT_DIRECT_SPEAKER;
    direct.element_id = 9U;
    direct.speaker_label = "M+030";
    direct.semantic_identity = &direct_identity;
    ok &= check(adm_scene_stream_configure_generation(stream.value, 52U, 1U, &direct, 1U) == ADM_ERROR_OK,
                "configure semantic DirectSpeakers element");
    constexpr const char* k_direct_policy =
        R"({"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_DS","direct_speakers":{"speaker_label":"M+030","gain":{"mute":true},"head_locked":true}}]})";
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_direct_policy, 12U) == ADM_ERROR_OK,
                "install DirectSpeakers semantic operation");
    ok &= wait_for_policy_revision(stream.value, 12U);
    std::vector<float> direct_samples(128U, 0.05F);
    adm_scene_pcm_plane_t direct_plane{};
    direct_plane.struct_size = sizeof(direct_plane);
    direct_plane.element_id = 9U;
    direct_plane.samples = direct_samples.data();
    direct_plane.sample_count = 128U;
    direct_plane.stride = 1U;
    direct_plane.has_signal = 1;
    adm_scene_initial_state_t direct_initial{};
    direct_initial.struct_size = sizeof(direct_initial);
    direct_initial.element_id = 9U;
    direct_initial.state = complete_state();
    frame = {};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 52U;
    frame.generation_id = 1U;
    frame.duration_samples = 128U;
    frame.pcm_count = 1U;
    frame.pcm = &direct_plane;
    frame.initial_state_count = 1U;
    frame.initial_states = &direct_initial;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit policy-controlled DirectSpeakers element");
    ok &=
        check(adm_scene_stream_signal_end(stream.value, 52U, 128) == ADM_ERROR_OK, "close DirectSpeakers policy epoch");
    saw_signal = false;
    output.clear();
    ok &= wait_for_output(stream.value, 2U, 128U, saw_signal, &output);
    ok &= check(!saw_signal, "filtered DirectSpeakers gain/mute operation is applied");
    return ok;
}

bool test_direct_speaker_policy_position_clear(adm_context_t* context) {
    StreamGuard stream;
    auto config = stream_config();
    config.output_ring_frames = 4096U;
    if (!create_stream(context, config, stream)) {
        return false;
    }

    bool ok = check(adm_scene_stream_begin_epoch(stream.value, 53U, 0) == ADM_ERROR_OK,
                    "begin DirectSpeakers position-policy epoch");
    adm_scene_semantic_identity_t identity{};
    identity.struct_size = sizeof(identity);
    identity.object_id = "AO_DS_POSITION";
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_DIRECT_SPEAKER;
    descriptor.element_id = 10U;
    descriptor.speaker_label = "M+030";
    descriptor.semantic_identity = &identity;
    ok &= check(adm_scene_stream_configure_generation(stream.value, 53U, 1U, &descriptor, 1U) == ADM_ERROR_OK,
                "configure labelled DirectSpeakers element without producer position");

    constexpr const char* k_position_policy =
        R"({"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_DS_POSITION","direct_speakers":{"speaker_label":"M+030","position":{"azimuth":-30}}}]})";
    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, k_position_policy, 13U) == ADM_ERROR_OK,
                "install DirectSpeakers position override");
    ok &= wait_for_policy_revision(stream.value, 13U);

    std::vector<float> samples(2048U, 0.05F);
    adm_scene_pcm_plane_t plane{};
    plane.struct_size = sizeof(plane);
    plane.element_id = 10U;
    plane.samples = samples.data();
    plane.sample_count = static_cast<std::uint32_t>(samples.size());
    plane.stride = 1U;
    plane.has_signal = 1;
    adm_scene_initial_state_t initial{};
    initial.struct_size = sizeof(initial);
    initial.element_id = 10U;
    initial.state = complete_state();
    initial.state.valid_fields &= ~static_cast<std::uint64_t>(ADM_SCENE_STATE_POSITION);
    adm_scene_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
    frame.epoch_id = 53U;
    frame.generation_id = 1U;
    frame.duration_samples = static_cast<std::uint32_t>(samples.size());
    frame.pcm_count = 1U;
    frame.pcm = &plane;
    frame.initial_state_count = 1U;
    frame.initial_states = &initial;
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit DirectSpeakers position-override block");
    std::vector<float> positioned;
    ok &= pull_open_stream(stream.value, 2U, 2048U, positioned);

    ok &= check(adm_scene_stream_set_semantic_policy_json(stream.value, nullptr, 14U) == ADM_ERROR_OK,
                "clear DirectSpeakers position override");
    ok &= wait_for_policy_revision(stream.value, 14U);
    frame.media_sample_start = 2048;
    frame.initial_state_count = 0U;
    frame.initial_states = nullptr;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit DirectSpeakers policy-clear transition block");
    std::vector<float> restored;
    ok &= pull_open_stream(stream.value, 2U, 2048U, restored);

    const auto positioned_ch0 = mean_absolute_channel_tail(positioned, 2U, 0U, 256U);
    const auto positioned_ch1 = mean_absolute_channel_tail(positioned, 2U, 1U, 256U);
    const auto restored_ch0 = mean_absolute_channel_tail(restored, 2U, 0U, 256U);
    const auto restored_ch1 = mean_absolute_channel_tail(restored, 2U, 1U, 256U);
    const bool positioned_uses_ch0 = positioned_ch0 > positioned_ch1;
    const bool restored_uses_ch0 = restored_ch0 > restored_ch1;
    ok &= check(std::max(positioned_ch0, positioned_ch1) > std::min(positioned_ch0, positioned_ch1) * 10.0F &&
                    std::max(restored_ch0, restored_ch1) > std::min(restored_ch0, restored_ch1) * 10.0F &&
                    positioned_uses_ch0 != restored_uses_ch0,
                "clearing a DirectSpeakers position policy removes the canonical override and restores label routing");

    ok &= check(adm_scene_stream_signal_end(stream.value, 53U, 4096) == ADM_ERROR_OK,
                "close DirectSpeakers position-policy epoch");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 0U, saw_signal);
    return ok;
}

bool render_binaural_pose(adm_context_t* context,
                          std::uint64_t epoch,
                          bool set_pose,
                          float yaw,
                          float pitch,
                          float roll,
                          bool head_locked,
                          std::vector<float>& output) {
    StreamGuard stream;
    auto config = stream_config();
    config.rendering.renderer = ADM_RENDERER_SAF_BINAURAL;
    config.rendering.output_layout = "binaural";
    config.output_ring_frames = 4096U;
    if (!create_stream(context, config, stream)) {
        return false;
    }
    bool ok = check(adm_scene_stream_begin_epoch(stream.value, epoch, 0) == ADM_ERROR_OK,
                    "begin posed binaural Scene epoch") &&
              configure_object(stream.value, epoch, 1U);
    if (set_pose) {
        ok &= check(adm_scene_stream_set_listener_orientation(stream.value, yaw, pitch, roll) == ADM_ERROR_OK,
                    "set live Scene listener yaw/pitch/roll");
    }
    std::vector<float> samples(512U, 0.1F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(epoch, 1U, 0, samples, plane, initial);
    initial.state.position_x = 0.0F;
    initial.state.position_y = 1.0F;
    initial.state.head_locked = head_locked ? 1 : 0;
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(stream.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit posed binaural SceneFrame");
    ok &= check(adm_scene_stream_signal_end(stream.value, epoch, 512) == ADM_ERROR_OK,
                "close posed binaural Scene epoch");
    bool saw_signal = false;
    ok &= wait_for_output(stream.value, 2U, 512U, saw_signal, &output);
    return ok && check(saw_signal, "posed binaural Scene produces PCM");
}

float maximum_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    if (lhs.size() != rhs.size()) {
        return std::numeric_limits<float>::infinity();
    }
    float result = 0.0F;
    for (std::size_t index = 0U; index < lhs.size(); ++index) {
        result = std::max(result, std::fabs(lhs[index] - rhs[index]));
    }
    return result;
}

// NOLINTNEXTLINE(readability-function-size)
bool test_listener_orientation(adm_context_t* context) {
    std::vector<float> baseline;
    std::vector<float> identity;
    std::vector<float> yaw_left;
    std::vector<float> yaw_right;
    std::vector<float> pitch_roll;
    std::vector<float> locked_baseline;
    std::vector<float> locked_pose;
    bool ok = render_binaural_pose(context, 42U, false, 0.0F, 0.0F, 0.0F, false, baseline);
    ok &= render_binaural_pose(context, 43U, true, 0.0F, 0.0F, 0.0F, false, identity);
    ok &= check(baseline == identity, "explicit zero listener pose remains bit-exact");
    ok &= render_binaural_pose(context, 44U, true, 90.0F, 0.0F, 0.0F, false, yaw_left);
    ok &= render_binaural_pose(context, 45U, true, -90.0F, 0.0F, 0.0F, false, yaw_right);
    ok &= check(maximum_difference(yaw_left, yaw_right) > 1.0e-6F,
                "opposite yaw poses produce opposite binaural directions");
    ok &= render_binaural_pose(context, 46U, true, 15.0F, 25.0F, -20.0F, false, pitch_roll);
    ok &= check(maximum_difference(baseline, pitch_roll) > 1.0e-6F,
                "pitch and roll participate in world-to-head rotation");
    ok &= render_binaural_pose(context, 47U, false, 0.0F, 0.0F, 0.0F, true, locked_baseline);
    ok &= render_binaural_pose(context, 48U, true, 80.0F, -30.0F, 25.0F, true, locked_pose);
    ok &= check(locked_baseline == locked_pose, "effective head_locked elements ignore listener rotation");
    ok &= check(adm_scene_stream_set_listener_orientation(nullptr, 0.0F, 0.0F, 0.0F) == ADM_ERROR_INVALID_ARGUMENT &&
                    adm_scene_stream_set_listener_orientation(
                        nullptr, std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F) == ADM_ERROR_INVALID_ARGUMENT,
                "listener-orientation C API validates handle and finite values");

    StreamGuard low_latency;
    auto config = stream_config();
    config.rendering.renderer = ADM_RENDERER_SAF_BINAURAL;
    config.rendering.output_layout = "binaural";
    config.output_ring_frames = 8192U;
    config.startup_watermark_frames = 4096U;
    ok &= create_stream(context, config, low_latency);
    ok &= check(adm_scene_stream_begin_epoch(low_latency.value, 49U, 0) == ADM_ERROR_OK,
                "begin low-lookahead head-tracking epoch");
    ok &= configure_object(low_latency.value, 49U, 1U);
    ok &= check(adm_scene_stream_set_listener_orientation(low_latency.value, 5.0F, 0.0F, 0.0F) == ADM_ERROR_OK,
                "activate low-lookahead head tracking");
    std::vector<float> samples(4096U, 0.05F);
    adm_scene_pcm_plane_t plane{};
    adm_scene_initial_state_t initial{};
    auto frame = object_frame(49U, 1U, 0, samples, plane, initial);
    int32_t submit = -1;
    ok &= check(adm_scene_stream_submit_frame(low_latency.value, &frame, 0U, &submit) == ADM_ERROR_OK &&
                    submit == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit maximum head-tracked SceneFrame");
    adm_scene_stream_status_t status{};
    bool reached_shallow_lead = false;
    for (int attempt = 0; attempt < 5000; ++attempt) {
        status = {};
        status.struct_size = sizeof(status);
        if (adm_scene_stream_get_status(low_latency.value, &status) == ADM_ERROR_OK &&
            status.buffered_output_frames >= 2048U) {
            reached_shallow_lead = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= check(reached_shallow_lead && status.buffered_output_frames <= 2048U && status.queued_input_samples == 4096U,
                "active head tracking caps worker lookahead at 2048 output frames");
    ok &= check(adm_scene_stream_signal_end(low_latency.value, 49U, 4096) == ADM_ERROR_OK,
                "close low-lookahead head-tracking epoch");
    bool saw_signal = false;
    std::vector<float> tracked_output;
    ok &= wait_for_output(low_latency.value, 2U, 4096U, saw_signal, &tracked_output);
    ok &= check(saw_signal, "low-lookahead stream drains without losing media frames");
    return ok;
}

} // namespace

int main() {
    adm_context_t* context = adm_create_context();
    bool ok = check(context != nullptr, "create C API context");
    if (context != nullptr) {
        ok &= test_deep_copy_and_timeline(context);
        ok &= test_validation_and_backpressure(context);
        ok &= test_resampling_length(context);
        ok &= test_resampling_preroll_boundary(context);
        ok &= test_rational_accumulator_many_frames(context);
        ok &= test_generation_topology_contract(context);
        ok &= test_zero_generation_id(context);
        ok &= test_sample_accurate_ramp(context);
        ok &= test_incomplete_generation_silence(context);
        ok &= test_pull_state_contract(context);
        ok &= test_worker_failure_pull(context);
        ok &= test_renderer_native_roles_and_events(context);
        ok &= test_binaural_backend(context);
        ok &= test_binaural_dynamic_position(context);
        ok &= test_backend_hot_switch(context);
        ok &= test_semantic_policy_hot_replace(context);
        ok &= test_semantic_identity_grouping_and_roles(context);
        ok &= test_direct_speaker_policy_position_clear(context);
        ok &= test_listener_orientation(context);
    }
    adm_destroy_context(context);
    if (ok) {
        std::cout << "scene_stream_c_api_test: ok\n";
    }
    return ok ? 0 : 1;
}
