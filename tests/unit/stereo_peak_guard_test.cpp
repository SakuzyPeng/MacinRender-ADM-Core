#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "../../src/adm_engine/scene_output_session.h"
#include "stereo_peak_guard.h"

namespace {
using namespace mradm::realtime;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

template <typename T> void require(const T& result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}

std::vector<float> process(std::span<const float> input, float volume, std::size_t block) {
    StereoPeakGuard guard(48000U);
    std::vector<float> output(input.size());
    std::size_t read = 0U;
    std::size_t written = 0U;
    while (read < input.size() || guard.buffered_frames() != 0U) {
        const auto count = std::min({block, (input.size() - read) / 2U, guard.writable_frames()});
        guard.push(input.subspan(read, count * 2U));
        read += count * 2U;
        const auto produced = guard.pop(std::span{output}.subspan(written), volume, read == input.size());
        written += produced * 2U;
        if (count == 0U && produced == 0U) {
            throw std::runtime_error("peak guard failed to make progress");
        }
    }
    if (written != input.size()) {
        throw std::runtime_error("peak guard changed the media length");
    }
    return output;
}

bool test_transparency_and_volume() {
    std::vector<float> input(2006U);
    for (std::size_t frame = 0U; frame < input.size() / 2U; ++frame) {
        input[frame * 2U] = 2.0F * std::sin(static_cast<float>(frame) * 0.073F);
        input[(frame * 2U) + 1U] = -0.5F * input[frame * 2U];
    }
    auto expected = input;
    std::ranges::transform(expected, expected.begin(), [](float sample) { return sample * 0.3F; });
    bool ok = true;
    for (const std::size_t block : {1U, 17U, 512U, 4096U}) {
        ok &= check(process(input, 0.3F, block) == expected,
                    "safe post-volume audio is unchanged, including short final buffers");
    }
    const auto muted = process(input, 0.0F, 257U);
    ok &= check(std::ranges::all_of(muted, [](float value) { return value == 0.0F; }), "mute remains exact");
    return ok;
}

bool test_linked_smooth_gain() {
    std::vector<float> input(6000U);
    for (std::size_t frame = 0U; frame < input.size() / 2U; ++frame) {
        input[frame * 2U] = frame == 720U ? 4.0F : 0.1F;
        input[(frame * 2U) + 1U] = input[frame * 2U] * 0.25F;
    }
    const auto reference = process(input, 0.8F, 4096U);
    bool ok = true;
    for (const std::size_t block : {1U, 37U, 480U}) {
        ok &= check(process(input, 0.8F, block) == reference, "limiting is independent of callback partitioning");
    }
    for (std::size_t frame = 0U; frame < input.size() / 2U; ++frame) {
        const auto left = reference[frame * 2U];
        const auto right = reference[(frame * 2U) + 1U];
        ok &= check(std::abs(left) <= StereoPeakGuard::k_ceiling + 1.0e-6F, "overload stays below the sample ceiling");
        ok &= check(right == left * 0.25F, "stereo gain preserves the interaural level ratio");
        if (frame > 0U && frame != 720U && frame != 721U) {
            ok &= check(std::abs(left - reference[(frame - 1U) * 2U]) < 0.001F,
                        "attack and release do not introduce block gain steps");
        }
    }
    ok &= check(reference[1200U] < 0.08F && reference[940U] == input[940U] * 0.8F,
                "gain reduction anticipates the overload within the 5 ms lookahead");
    return ok;
}

bool test_short_end_and_reset() {
    StereoPeakGuard guard(48000U);
    std::vector<float> loud(64U, 4.0F);
    guard.push(loud);
    std::vector<float> output(64U);
    bool ok = check(guard.pop(output, 1.0F, false) == 0U, "short input waits for lookahead or EOS");
    ok &= check(guard.pop(output, 1.0F, true) == 32U && guard.buffered_frames() == 0U,
                "EOS drains every remaining media frame without appending padding");
    guard.push(loud);
    guard.reset();
    std::vector<float> quiet(34U, 0.1F);
    guard.push(quiet);
    ok &= check(guard.pop(output, 1.0F, true) == 17U &&
                    std::ranges::all_of(std::span{output}.first(34U), [](float value) { return value == 0.1F; }),
                "seek discards buffered audio and old gain reduction");
    std::vector<float> invalid{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    const auto finite = process(invalid, 1.0F, 1U);
    ok &= check(finite[0U] == 0.0F && finite[1U] == 0.0F, "non-finite input cannot contaminate the device");
    return ok;
}

class CaptureDevice final : public IAudioOutputDevice {
  public:
    mradm::Result<void> start(std::uint32_t /*channels*/, std::uint32_t rate, PullFn pull) override {
        rate_ = rate;
        pull_ = std::move(pull);
        return {};
    }
    void stop() override { pull_ = {}; }
    [[nodiscard]] std::uint32_t actual_sample_rate() const override { return rate_; }
    std::size_t pull(std::span<float> output) { return pull_(output, output.size() / 2U); }

  private:
    PullFn pull_;
    std::uint32_t rate_{0U};
};

bool test_device_clock_and_epoch() {
    SceneStreamConfig config;
    config.renderer.renderer = mradm::RendererSelection::saf_binaural;
    config.renderer.output_layout = "binaural";
    config.startup_watermark_frames = 1U;
    auto created = SceneStreamEngine::create(config);
    require(created);
    auto stream = std::shared_ptr<SceneStreamEngine>(std::move(*created));
    auto device = std::make_unique<CaptureDevice>();
    auto* capture = device.get();
    auto output = SceneOutputSession::create_with_device(stream, std::move(device), true);
    require(output);
    require((*output)->set_volume(0.8F));
    bool ok = true;
    for (const std::uint32_t frames : {4417U, 17U}) {
        const auto epoch = frames == 4417U ? 1U : 2U;
        require((*output)->begin_epoch(epoch, 0));
        mradm::live_scene::ElementDescriptor element;
        element.element_id = 1U;
        element.role = mradm::live_scene::ElementRole::lfe;
        require(stream->configure_generation(epoch, 1U, std::span{&element, 1U}));
        std::vector<float> samples(frames, epoch == 1U ? 3.0F : 0.1F);
        ScenePcmPlaneView plane{1U, samples.data(), frames, 1U, true};
        SceneFrameView frame;
        frame.epoch_id = epoch;
        frame.generation_id = 1U;
        frame.duration_samples = frames;
        frame.flags = mradm::live_scene::frame_state_complete;
        frame.pcm = std::span{&plane, 1U};
        require(stream->submit_frame(frame, std::chrono::milliseconds(100)));
        require(stream->signal_end(epoch, frames));
        (*output)->play();
        std::vector<float> callback(960U);
        std::size_t count = 0U;
        bool checked_pause = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!(*output)->status().ended && std::chrono::steady_clock::now() < deadline) {
            const auto produced = capture->pull(callback);
            count += produced;
            for (const float sample : std::span{callback}.first(produced * 2U)) {
                ok &= check(std::abs(sample) <= StereoPeakGuard::k_ceiling + 1.0e-6F,
                            "device callback receives protected PCM after master volume");
                if (epoch == 2U) {
                    ok &= check(std::abs(sample) <= 0.080001F, "new epoch contains no old lookahead audio");
                }
            }
            if (epoch == 1U && count >= 480U && !checked_pause) {
                (*output)->pause();
                ok &= check(capture->pull(callback) == 0U && (*output)->status().consumed_frames == count,
                            "pause retains lookahead without consuming media");
                (*output)->play();
                checked_pause = true;
            }
            if (produced == 0U) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        const auto status = (*output)->status();
        ok &= check(status.ended && count == frames && status.consumed_frames == frames &&
                        status.presented_frames == frames,
                    "lookahead preserves exact output media length and final presentation clock");
    }
    return ok;
}

bool test_real_pcm() {
    const char* path = std::getenv("MR_ADM_TEST_STEREO_PCM");
    if (path == nullptr) {
        return true;
    }
    const auto bytes = std::filesystem::file_size(path);
    if (bytes == 0U || bytes % (2U * sizeof(float)) != 0U) {
        throw std::runtime_error("stereo fixture must be interleaved native float32 PCM");
    }
    std::vector<float> input(static_cast<std::size_t>(bytes / sizeof(float)));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(input.data()), static_cast<std::streamsize>(bytes));
    if (!file) {
        throw std::runtime_error("cannot read stereo PCM fixture");
    }
    const auto output = process(input, 0.8F, 480U);
    const auto peak = std::ranges::max(output, {}, [](float sample) { return std::abs(sample); });
    std::cout << "real PCM: " << output.size() / 2U << " frames, post-volume peak=" << std::abs(peak) << '\n';
    if (const char* destination = std::getenv("MR_ADM_TEST_STEREO_OUTPUT")) {
        std::ofstream result(destination, std::ios::binary);
        result.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(bytes));
    }
    return check(std::abs(peak) <= StereoPeakGuard::k_ceiling + 1.0e-6F,
                 "real custom-SOFA output no longer reaches device hard clipping");
}
} // namespace

int main() {
    try {
        bool ok = test_transparency_and_volume();
        ok &= test_linked_smooth_gain();
        ok &= test_short_end_and_reset();
        ok &= test_device_clock_and_epoch();
        ok &= test_real_pcm();
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
