#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adm/c_api.h"
#include "adm/hptf.h"

#include "../../src/adm_engine/scene_output_session.h"
#include "hptf_eq.h"

namespace {
constexpr const char* k_text = "Preamp: -6 dB\n"
                               "Filter 1: ON LSC Fc 105 Hz Gain 3 dB Q 0.7\n"
                               "Filter 2: ON PK Fc 1000 Hz Gain -2 dB Q 1.5\n"
                               "Filter 3: OFF HSC Fc 10000 Hz Gain 1 dB Q 0.7\n";

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T> T unwrap(mradm::Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

void require_ok(const mradm::Result<void>& result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}

using Context = std::unique_ptr<adm_context_t, decltype(&adm_destroy_context)>;
using Stream = std::unique_ptr<adm_scene_stream_t, decltype(&adm_destroy_scene_stream)>;
using Output = std::unique_ptr<adm_scene_output_t, decltype(&adm_destroy_scene_output)>;

adm_hptf_band_t band(double fc = 1000.0) {
    return {sizeof(adm_hptf_band_t), ADM_HPTF_BAND_PEAKING, 1, 0, fc, 3.0, 1.0};
}

adm_hptf_parameters_t parameters(std::span<const adm_hptf_band_t> bands, uint64_t revision = 1) {
    return {sizeof(adm_hptf_parameters_t),
            static_cast<uint32_t>(bands.size()),
            bands.data(),
            -6.0,
            ADM_HPTF_PREAMP_WARN_ONLY,
            0,
            revision};
}

class ApiScene {
  public:
    ApiScene() {
        require(context_ != nullptr, "context creation");
        adm_scene_stream_config_t config{};
        config.struct_size = sizeof(config);
        config.rendering.struct_size = sizeof(config.rendering);
        config.rendering.renderer = ADM_RENDERER_SAF_BINAURAL;
        config.rendering.output_layout = "binaural";
        config.input_sample_rate = 48000;
        config.output_sample_rate = 48000;
        config.input_queue_samples = 96000;
        config.startup_watermark_frames = 1;
        adm_scene_stream_t* stream = nullptr;
        require(adm_create_scene_stream(context_.get(), &config, &stream) == ADM_ERROR_OK, "create memory Scene");
        stream_.reset(stream);
        adm_scene_output_config_t device{};
        device.struct_size = sizeof(device);
        device.kind = ADM_SCENE_OUTPUT_NULL;
        adm_scene_output_t* output = nullptr;
        require(adm_create_scene_output(context_.get(), stream_.get(), &device, &output) == ADM_ERROR_OK,
                "create headless stereo output");
        output_.reset(output);
    }

    [[nodiscard]] adm_scene_output_t* output() const { return output_.get(); }
    [[nodiscard]] adm_context_t* context() const { return context_.get(); }

    void activate() {
        require(adm_scene_output_begin_epoch(output(), ++epoch_, 0) == ADM_ERROR_OK, "activate pending parameters");
    }

    [[nodiscard]] adm_hptf_info_t info() const {
        adm_hptf_info_t result{};
        result.struct_size = sizeof(result);
        require(adm_scene_output_get_hptf_info(output(), &result) == ADM_ERROR_OK, "read applied parameters");
        return result;
    }

    void play_generated_signal() {
        activate();
        adm_scene_element_descriptor_t element{};
        element.struct_size = sizeof(element);
        element.element_id = 1;
        element.role = ADM_SCENE_ELEMENT_LFE;
        require(adm_scene_stream_configure_generation(stream_.get(), epoch_, 1, &element, 1) == ADM_ERROR_OK,
                "configure generated signal");
        std::vector<float> samples(48000, 0.05F);
        adm_scene_pcm_plane_t plane{};
        plane.struct_size = sizeof(plane);
        plane.element_id = 1;
        plane.samples = samples.data();
        plane.sample_count = static_cast<uint32_t>(samples.size());
        plane.stride = 1;
        plane.has_signal = 1;
        adm_scene_frame_t frame{};
        frame.struct_size = sizeof(frame);
        frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
        frame.epoch_id = epoch_;
        frame.generation_id = 1;
        frame.duration_samples = plane.sample_count;
        frame.pcm_count = 1;
        frame.pcm = &plane;
        int32_t accepted = -1;
        require(adm_scene_stream_submit_frame(stream_.get(), &frame, 100, &accepted) == ADM_ERROR_OK &&
                    accepted == ADM_SCENE_SUBMIT_ACCEPTED,
                "submit generated signal");
        require(adm_scene_stream_signal_end(stream_.get(), epoch_, frame.duration_samples) == ADM_ERROR_OK,
                "signal generated EOS");
        require(adm_scene_output_play(output()) == ADM_ERROR_OK, "start null output");
    }

  private:
    Context context_{adm_create_context(), adm_destroy_context};
    Stream stream_{nullptr, adm_destroy_scene_stream};
    Output output_{nullptr, adm_destroy_scene_output};
    uint64_t epoch_{0};
};

void test_parser_buffers() {
    Context context(adm_create_context(), adm_destroy_context);
    double preamp = 99.0;
    uint32_t count = 99;
    require(adm_hptf_parse_parametric_eq(context.get(), k_text, &preamp, nullptr, 0, &count) == ADM_ERROR_OK &&
                count == 3 && preamp == -6.0,
            "sizing query returns all bands, including disabled entries");
    std::array<adm_hptf_band_t, 3> bands{band(), band(), band()};
    require(adm_hptf_parse_parametric_eq(context.get(), k_text, &preamp, bands.data(), 2, &count) ==
                    ADM_ERROR_INVALID_ARGUMENT &&
                count == 3 && bands[0].fc_hz == 1000.0 && bands[1].gain_db == 3.0,
            "insufficient capacity reports needed size without partial output");
    require(adm_hptf_parse_parametric_eq(context.get(), k_text, &preamp, bands.data(), 3, &count) == ADM_ERROR_OK,
            "parse into caller-owned buffers");
    const auto parsed = unwrap(mradm::parse_hptf_parametric_eq(k_text));
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const auto& actual = bands.at(i);
        const auto& expected = parsed.bands[i];
        require(actual.struct_size == sizeof(actual) && actual.type == static_cast<int32_t>(expected.type) &&
                    actual.enabled == static_cast<int32_t>(expected.enabled) && actual.fc_hz == expected.fc_hz &&
                    actual.gain_db == expected.gain_db && actual.q == expected.q,
                "C and C++ parsing expose the same editable parameters");
    }
    bands[1].struct_size = 4;
    count = 77;
    preamp = 77;
    require(adm_hptf_parse_parametric_eq(context.get(), k_text, &preamp, bands.data(), 3, &count) ==
                    ADM_ERROR_INVALID_ARGUMENT &&
                count == 77 && preamp == 77 && bands[0].fc_hz == 105.0,
            "invalid output stride leaves all outputs unchanged");
    require(adm_hptf_parse_parametric_eq(context.get(), "Preamp: bad", &preamp, nullptr, 0, &count) ==
                    ADM_ERROR_INVALID_ARGUMENT &&
                count == 77 && preamp == 77,
            "parse failure preserves outputs");
    require(adm_context_last_error_message(context.get())[0] != '\0', "parser returns diagnostics");
    require(adm_hptf_parse_parametric_eq(nullptr, k_text, &preamp, nullptr, 0, &count) == ADM_ERROR_INVALID_ARGUMENT,
            "null context rejected");
    require(adm_hptf_parse_parametric_eq(context.get(), k_text, &preamp, nullptr, 1, &count) ==
                ADM_ERROR_INVALID_ARGUMENT,
            "null output with nonzero capacity rejected");
}

struct FutureBand {
    adm_hptf_band_t value{};
    std::array<uint64_t, 2> tail{0x123456789abcdef0ULL, 0xfedcba9876543210ULL};
};

void test_future_layouts_and_validation() {
    ApiScene scene;
    std::array<FutureBand, 3> bands{};
    for (auto& value : bands) {
        value.value.struct_size = sizeof(FutureBand);
    }
    double preamp = 0;
    uint32_t count = 0;
    require(adm_hptf_parse_parametric_eq(scene.context(), k_text, &preamp, &bands[0].value, 3, &count) == ADM_ERROR_OK,
            "parser accepts future output strides");
    for (const auto& value : bands) {
        require(value.value.struct_size == sizeof(FutureBand) && value.tail == FutureBand{}.tail,
                "parser preserves caller size and extension bytes");
    }
    struct FutureParameters {
        adm_hptf_parameters_t value;
        uint64_t tail;
    } request{{sizeof(FutureParameters), 3, &bands[0].value, preamp, ADM_HPTF_PREAMP_WARN_ONLY, 0, 10}, 123};
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request.value) == ADM_ERROR_OK,
            "setter accepts future parameter and array layouts");
    scene.activate();
    const auto before = scene.info();
    require(before.band_count == 2 && before.preamp_db == -6.0F && before.applied_revision == 10,
            "future layout is read with the caller stride");
    bands[1].value.q = std::numeric_limits<double>::quiet_NaN();
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request.value) == ADM_ERROR_INVALID_ARGUMENT,
            "second element is validated at the future stride");
    require(scene.info().applied_revision == 10, "invalid update preserves applied revision");
    // cppcheck-suppress redundantAssignment; the previous NaN was read through request.value.bands.
    bands[1].value.q = 1.5;
    // cppcheck-suppress unreadVariable; the C ABI reads this through request.value.bands.
    bands[2].value.type = 99;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request.value) == ADM_ERROR_INVALID_ARGUMENT,
            "disabled bands still require valid types");
    auto simple = parameters({});
    simple.band_count = 1;
    simple.bands = nullptr;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &simple) == ADM_ERROR_INVALID_ARGUMENT,
            "null input array with a nonzero count rejected");
    simple.band_count = 0;
    simple.preamp_mode = 99;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &simple) == ADM_ERROR_INVALID_ARGUMENT,
            "unknown preamp mode rejected");
    simple.preamp_mode = ADM_HPTF_PREAMP_WARN_ONLY;
    simple.preamp_db = 800;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &simple) == ADM_ERROR_INVALID_ARGUMENT,
            "unrepresentable gain rejected before publication");
    simple.struct_size = sizeof(uint32_t);
    require(adm_scene_output_set_hptf_parameters(scene.output(), &simple) == ADM_ERROR_INVALID_ARGUMENT,
            "undersized parameters rejected");
    require(adm_monitor_set_hptf_parameters(nullptr, &simple) == ADM_ERROR_INVALID_ARGUMENT,
            "monitor memory entry point rejects null handle");
}

void test_owned_snapshots_and_rapid_updates() {
    ApiScene scene;
    std::vector<adm_hptf_band_t> bands{band(), band(2000.0)};
    auto request = parameters(bands, 20);
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request) == ADM_ERROR_OK, "apply memory snapshot");
    require(scene.info().applied_revision == 0, "accepted target is distinct from applied state");
    bands.clear();
    bands.shrink_to_fit();
    request.preamp_db = 99;
    scene.activate();
    require(scene.info().band_count == 2 && scene.info().preamp_db == -6.0F && scene.info().applied_revision == 20,
            "caller may release arrays and edit descriptors immediately after return");

    bands.assign(33, band());
    request = parameters(bands, 21);
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request) == ADM_ERROR_INVALID_ARGUMENT,
            "more than 32 enabled bands rejected");
    bands.back().enabled = 0;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request) == ADM_ERROR_OK,
            "disabled bands do not consume the 32-section budget");
    scene.activate();
    require(scene.info().band_count == 32, "32 enabled bands applied");
    request = parameters({}, 22);
    request.preamp_db = -12;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request) == ADM_ERROR_OK, "preamp-only update");
    scene.activate();
    require(scene.info().enabled == 1 && scene.info().band_count == 0 && scene.info().preamp_db == -12,
            "zero bands still apply preamp");
    request.preamp_db = 0;
    request.revision = 23;
    require(adm_scene_output_set_hptf_parameters(scene.output(), &request) == ADM_ERROR_OK, "bypass update");
    scene.activate();
    require(scene.info().enabled == 0 && scene.info().applied_revision == 23, "memory bypass applied");

    scene.play_generated_signal();
    bands.assign(1, band());
    request = parameters(bands);
    for (uint64_t revision = 100; revision <= 180; ++revision) {
        request.revision = revision;
        request.preamp_db = -6.0 - static_cast<double>(revision % 4U);
        bands[0].fc_hz = 700.0 + static_cast<double>(revision);
        require(adm_scene_output_set_hptf_parameters(scene.output(), &request) == ADM_ERROR_OK,
                "rapid live parameter updates accepted");
        if (revision % 8U == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
    bands.clear();
    bands.shrink_to_fit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (scene.info().applied_revision != 180 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    require(scene.info().applied_revision == 180 && scene.info().preamp_db == -6,
            "latest complete snapshot wins while the null output keeps playing");
}

class CaptureDevice final : public mradm::realtime::IAudioOutputDevice {
  public:
    mradm::Result<void> start(uint32_t /*channels*/, uint32_t rate, PullFn pull) override {
        rate_ = rate;
        pull_ = std::move(pull);
        return {};
    }
    void stop() override { pull_ = {}; }
    [[nodiscard]] uint32_t actual_sample_rate() const override { return rate_; }
    std::size_t pull(std::span<float> output) { return pull_(output, output.size() / 2U); }

  private:
    uint32_t rate_{0};
    PullFn pull_;
};

std::vector<float> render_profile(const mradm::HptfProfile& profile, const std::string& path = {}) {
    using namespace mradm::realtime;
    SceneStreamConfig config;
    config.renderer.renderer = mradm::RendererSelection::saf_binaural;
    config.renderer.output_layout = "binaural";
    config.startup_watermark_frames = 1;
    auto stream = std::shared_ptr<SceneStreamEngine>(unwrap(SceneStreamEngine::create(config)));
    auto device = std::make_unique<CaptureDevice>();
    auto* capture = device.get();
    auto output = unwrap(SceneOutputSession::create_with_device(stream, std::move(device), true));
    if (path.empty()) {
        auto editable = profile;
        require_ok(output->set_hptf_parameters(editable, mradm::HptfPreampMode::warn_only, 1));
        editable.bands.clear();
        // cppcheck-suppress unreadVariable; deliberately poison caller memory after the setter returns.
        editable.preamp_db = 100;
    } else {
        require_ok(output->set_hptf_profile(path, mradm::HptfPreampMode::warn_only, 1));
    }
    require_ok(output->begin_epoch(1, 0));
    mradm::live_scene::ElementDescriptor element;
    element.element_id = 1;
    element.role = mradm::live_scene::ElementRole::lfe;
    require_ok(stream->configure_generation(1, 1, std::span{&element, 1U}));
    std::vector<float> samples(4096);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(0.1 * std::sin(static_cast<double>(i) * 0.13));
    }
    ScenePcmPlaneView plane{1, samples.data(), static_cast<uint32_t>(samples.size()), 1, true};
    SceneFrameView frame;
    frame.epoch_id = 1;
    frame.generation_id = 1;
    frame.duration_samples = static_cast<uint32_t>(samples.size());
    frame.flags = mradm::live_scene::frame_state_complete;
    frame.pcm = std::span{&plane, 1U};
    unwrap(stream->submit_frame(frame, std::chrono::milliseconds{100}));
    require_ok(stream->signal_end(1, static_cast<std::int64_t>(samples.size())));
    output->play();
    std::array<float, 512> buffer{};
    std::vector<float> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!output->status().ended && std::chrono::steady_clock::now() < deadline) {
        const auto count = capture->pull(buffer);
        const auto produced = std::span{buffer}.first(count * 2U);
        result.insert(result.end(), produced.begin(), produced.end());
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    require(output->status().ended && result.size() == samples.size() * 2U, "captured exact output media length");
    return result;
}

void test_concurrent_file_api_owns_errors() {
    ApiScene scene;
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("mradm-hptf-ex-" + std::to_string(serial) + ".txt");
    const auto name = path.string();
    {
        std::ofstream file(path);
        file << k_text;
        require(static_cast<bool>(file), "write concurrent import fixture");
    }
    adm_hptf_config_t config{sizeof(adm_hptf_config_t), name.c_str(), ADM_HPTF_PREAMP_WARN_ONLY, 0, 71};
    char* error = nullptr;
    const auto code = adm_scene_output_set_hptf_ex(scene.output(), &config, &error);
    std::filesystem::remove(path);
    require(code == ADM_ERROR_OK && error == nullptr, "concurrent setter accepts a file");
    scene.activate();
    require(scene.info().applied_revision == 71 && scene.info().band_count == 2, "concurrent setter applies profile");
    require(adm_scene_output_set_hptf_ex(scene.output(), &config, &error) == ADM_ERROR_IO && error != nullptr,
            "missing file returns an owned error");
    const std::unique_ptr<char, decltype(&adm_free_string)> owned(error, adm_free_string);
    const std::string message{owned.get()};
    adm_scene_output_status_t status{};
    status.struct_size = sizeof(status);
    require(adm_scene_output_get_status(scene.output(), &status) == ADM_ERROR_OK, "status clears borrowed error");
    // Check the ABI ownership contract: a mistakenly borrowed error would be cleared by the call above.
    // cppcheck-suppress knownConditionTrueFalse
    require(message == owned.get(), "status queries cannot invalidate the setter's error");
    require(scene.info().applied_revision == 71, "failed setter preserves the old profile");
    require(adm_scene_output_set_hptf_ex(scene.output(), &config, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
            "owned error slot is required");
    error = owned.get();
    require(adm_scene_output_set_hptf_ex(nullptr, &config, &error) == ADM_ERROR_INVALID_ARGUMENT && error == nullptr,
            "invalid handle clears the error slot");
    config.profile_path = "";
    config.revision = 72;
    require(adm_scene_output_set_hptf_ex(scene.output(), &config, &error) == ADM_ERROR_OK && error == nullptr,
            "concurrent setter accepts bypass");
    scene.activate();
    require(scene.info().applied_revision == 72 && scene.info().enabled == 0, "bypass is acknowledged");
}

void test_file_text_and_memory_equivalence() {
    const auto parsed = unwrap(mradm::parse_hptf_parametric_eq(k_text));
    mradm::HptfProfile editable;
    editable.preamp_db = -6;
    editable.bands = {{mradm::HptfBandType::low_shelf, true, 105, 3, 0.7},
                      {mradm::HptfBandType::peaking, true, 1000, -2, 1.5},
                      {mradm::HptfBandType::high_shelf, false, 10000, 1, 0.7}};
    const auto expected = render_profile(editable);
    require(std::ranges::any_of(expected, [](float value) { return std::abs(value) > 1e-6F; }),
            "equivalence fixture contains audible nonzero PCM");
    require(render_profile(parsed) == expected,
            "text import and directly constructed parameters produce identical PCM");
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("mradm-hptf-memory-" + std::to_string(serial) + ".txt");
    {
        std::ofstream file(path);
        file << k_text;
        require(static_cast<bool>(file), "write legacy import fixture");
    }
    const auto from_file = render_profile({}, path.string());
    std::filesystem::remove(path);
    require(from_file == expected, "legacy file adapter produces identical PCM");
}
} // namespace

int main() {
    try {
        test_parser_buffers();
        test_future_layouts_and_validation();
        test_owned_snapshots_and_rapid_updates();
        test_file_text_and_memory_equivalence();
        test_concurrent_file_api_owns_errors();
        std::cout << "HpTF memory parameter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
