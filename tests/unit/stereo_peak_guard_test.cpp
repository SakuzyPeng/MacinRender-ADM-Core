#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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
#include "../../src/adm_render_common/hptf_eq.h"
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

// Without a peak guard the session applies its own volume to every rendered sample.
std::vector<float> render_unprotected(float volume) {
    SceneStreamConfig config;
    config.renderer.renderer = mradm::RendererSelection::saf_binaural;
    config.renderer.output_layout = "binaural";
    config.startup_watermark_frames = 1U;
    auto created = SceneStreamEngine::create(config);
    require(created);
    auto stream = std::shared_ptr<SceneStreamEngine>(std::move(*created));
    auto device = std::make_unique<CaptureDevice>();
    auto* capture = device.get();
    auto output = SceneOutputSession::create_with_device(stream, std::move(device), false);
    require(output);
    require((*output)->set_volume(volume));
    constexpr std::uint32_t k_frames = 1531U;
    require((*output)->begin_epoch(1U, 0));
    mradm::live_scene::ElementDescriptor element;
    element.element_id = 1U;
    element.role = mradm::live_scene::ElementRole::lfe;
    require(stream->configure_generation(1U, 1U, std::span{&element, 1U}));
    std::vector<float> samples(k_frames);
    for (std::size_t i = 0U; i < samples.size(); ++i) {
        samples[i] = std::sin(static_cast<float>(i) * 0.01F) * 1.5F;
    }
    ScenePcmPlaneView plane{1U, samples.data(), k_frames, 1U, true};
    SceneFrameView frame;
    frame.epoch_id = 1U;
    frame.generation_id = 1U;
    frame.duration_samples = k_frames;
    frame.flags = mradm::live_scene::frame_state_complete;
    frame.pcm = std::span{&plane, 1U};
    require(stream->submit_frame(frame, std::chrono::milliseconds(100)));
    require(stream->signal_end(1U, k_frames));
    (*output)->play();
    std::vector<float> result;
    std::vector<float> callback(254U);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(*output)->status().ended && std::chrono::steady_clock::now() < deadline) {
        const auto produced = capture->pull(callback);
        result.insert(result.end(), callback.begin(), callback.begin() + static_cast<std::ptrdiff_t>(produced * 2U));
        if (produced == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (result.size() != std::size_t{k_frames} * 2U) {
        throw std::runtime_error("unprotected session changed the media length");
    }
    return result;
}

bool test_unprotected_volume() {
    const auto unity = render_unprotected(1.0F);
    const auto scaled = render_unprotected(0.375F);
    bool exact = std::ranges::any_of(unity, [](float sample) { return std::abs(sample) > 1.0F; });
    for (std::size_t i = 0U; i < unity.size(); ++i) {
        exact &= scaled[i] == unity[i] * 0.375F;
    }
    return check(exact, "unprotected output applies the session volume to unbounded PCM");
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

// A big low shelf, so even low-frequency / near-DC content gets boosted hard. A peaking band
// would leave such content almost untouched and would not exercise the ceiling.
mradm::render_common::HptfCoefficients hptf_low_boost() {
    auto profile = mradm::render_common::parse_parametric_eq("Preamp: 0 dB\n"
                                                             "Filter 1: ON LSC Fc 200 Hz Gain 12 dB Q 0.7\n");
    return *mradm::render_common::design_cascade(*profile, 48000U, mradm::HptfPreampMode::warn_only);
}

// HpTF is applied UPSTREAM of StereoPeakGuard, so a boosted band must still be caught by the
// -1 dBFS ceiling. This is the assertion that pins the insertion point: move the HpTF stage
// downstream of peak_guard_->push() and a +12 dB shelf escapes the guard, failing here.
bool test_scene_hptf_respects_ceiling() {
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

    bool ok = true;
    ok &= check((*output)->hptf_supported(), "scene hptf: stereo feed supports HpTF");
    require((*output)->set_hptf(hptf_low_boost(), 5));

    constexpr std::uint32_t k_frames = 8000U;
    require((*output)->begin_epoch(1U, 0));
    mradm::live_scene::ElementDescriptor element;
    element.element_id = 1U;
    element.role = mradm::live_scene::ElementRole::lfe;
    require(stream->configure_generation(1U, 1U, std::span{&element, 1U}));
    // Deliberately hot source material: without the guard a +12 dB shelf on top of this is far
    // above full scale.
    std::vector<float> samples(k_frames, 3.0F);
    ScenePcmPlaneView plane{1U, samples.data(), k_frames, 1U, true};
    SceneFrameView frame;
    frame.epoch_id = 1U;
    frame.generation_id = 1U;
    frame.duration_samples = k_frames;
    frame.flags = mradm::live_scene::frame_state_complete;
    frame.pcm = std::span{&plane, 1U};
    require(stream->submit_frame(frame, std::chrono::milliseconds(100)));
    require(stream->signal_end(1U, k_frames));

    (*output)->play();
    std::vector<float> callback(960U);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(*output)->status().ended && std::chrono::steady_clock::now() < deadline) {
        const auto produced = capture->pull(callback);
        for (const float sample : std::span{callback}.first(produced * 2U)) {
            ok &= check(std::abs(sample) <= StereoPeakGuard::k_ceiling + 1.0e-6F,
                        "scene hptf: 提升后的信号仍被 -1 dBFS 天花板挡住(HpTF 在限幅器上游)");
        }
        if (produced == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ok &= check((*output)->hptf_applied_revision() == 5, "scene hptf: applied revision published");
    return ok;
}

// A non-stereo / system-spatial session hands a multichannel bed to the OS for HRTF, so there is
// no 2ch headphone signal on our side. Reject rather than silently ignore.
bool test_scene_hptf_unsupported_without_stereo_guard() {
    SceneStreamConfig config;
    config.renderer.renderer = mradm::RendererSelection::saf_binaural;
    config.renderer.output_layout = "binaural";
    config.startup_watermark_frames = 1U;
    auto created = SceneStreamEngine::create(config);
    require(created);
    auto stream = std::shared_ptr<SceneStreamEngine>(std::move(*created));
    auto device = std::make_unique<CaptureDevice>();
    // protect_stereo = false is the system-spatial path: no peak guard, and no HpTF either.
    auto output = SceneOutputSession::create_with_device(stream, std::move(device), false);
    require(output);

    bool ok = true;
    ok &= check(!(*output)->hptf_supported(), "scene hptf: 非立体声输出不支持 HpTF");
    const auto res = (*output)->set_hptf(hptf_low_boost(), 1);
    ok &= check(!res.has_value() && res.error().code == mradm::ErrorCode::unsupported,
                "scene hptf: 非立体声输出返回 unsupported");
    return ok;
}

bool test_scene_hptf_snapshot_on_epoch() {
    SceneStreamConfig config;
    config.renderer.renderer = mradm::RendererSelection::saf_binaural;
    config.renderer.output_layout = "binaural";
    auto created = SceneStreamEngine::create(config);
    require(created);
    auto stream = std::shared_ptr<SceneStreamEngine>(std::move(*created));
    auto output = SceneOutputSession::create_with_device(stream, std::make_unique<CaptureDevice>(), true);
    require(output);
    auto profile = mradm::render_common::parse_parametric_eq("Preamp: -6 dB\n");
    require(profile);
    auto coeffs = mradm::render_common::design_cascade(*profile, 48000U, mradm::HptfPreampMode::warn_only);
    require(coeffs);
    require((*output)->set_hptf(*coeffs, 9));
    const auto pending = (*output)->hptf_info();
    bool ok = check(!pending.enabled && pending.preamp_db == 0.0F && pending.applied_revision == 0,
                    "pending preamp does not leak into applied status");
    require((*output)->begin_epoch(1U, 0));
    const auto applied = (*output)->hptf_info();
    ok &= check(applied.enabled && applied.band_count == 0 && applied.preamp_db == -6.0F &&
                    applied.applied_revision == 9 && applied.sample_rate == 48000U,
                "epoch reset applies a preamp-only profile with coherent metadata");
    require((*output)->set_hptf(std::nullopt, 10));
    require((*output)->begin_epoch(2U, 0));
    const auto bypass = (*output)->hptf_info();
    ok &= check(!bypass.enabled && bypass.preamp_db == 0.0F && bypass.applied_revision == 10,
                "epoch reset applies pending bypass");
    return ok;
}

mradm::render_common::HptfCoefficients hptf_design(const char* text) {
    auto profile = mradm::render_common::parse_parametric_eq(text);
    require(profile);
    auto coeffs = mradm::render_common::design_cascade(*profile, 48000U, mradm::HptfPreampMode::warn_only);
    require(coeffs);
    return *coeffs;
}

// Output controls are applied at consumed device frames. With the Scene output
// fully produced, HpTF, peak guard and volume depend only on those positions,
// not on callback sizes. Revision 2 arrives during the fade to 1 and is
// replaced by 3 before that fade ends.
std::vector<float> render_device_partition(std::span<const std::size_t> sizes, std::vector<std::uint64_t>& revisions) {
    constexpr std::uint32_t k_frames = 12000U;
    SceneStreamConfig config;
    config.renderer.renderer = mradm::RendererSelection::saf;
    config.renderer.output_layout = "0+2+0";
    config.startup_watermark_frames = 1U;
    config.output_ring_frames = 16384U;
    auto created = SceneStreamEngine::create(config);
    require(created);
    auto stream = std::shared_ptr<SceneStreamEngine>(std::move(*created));
    auto device = std::make_unique<CaptureDevice>();
    auto* capture = device.get();
    auto output = SceneOutputSession::create_with_device(stream, std::move(device), true);
    require(output);
    require((*output)->begin_epoch(1U, 0));
    mradm::live_scene::ElementDescriptor element;
    element.element_id = 1U;
    require(stream->configure_generation(1U, 1U, std::span{&element, 1U}));
    std::vector<float> samples(k_frames);
    std::uint32_t seed = 0x2468ACE1U;
    for (auto& sample : samples) {
        seed = (seed * 1664525U) + 1013904223U;
        sample = 3.0F * (static_cast<float>(seed >> 8U) / 8388608.0F - 1.0F);
    }
    mradm::live_scene::StateEntry initial;
    initial.element_id = 1U;
    initial.state.valid_fields = mradm::live_scene::k_known_state_fields;
    initial.state.x = 0.25F;
    initial.state.y = 0.75F;
    ScenePcmPlaneView plane{1U, samples.data(), k_frames, 1U, true};
    SceneFrameView frame;
    frame.epoch_id = 1U;
    frame.generation_id = 1U;
    frame.duration_samples = k_frames;
    frame.flags = mradm::live_scene::frame_state_complete;
    frame.pcm = std::span{&plane, 1U};
    frame.initial_states = std::span{&initial, 1U};
    require(stream->submit_frame(frame, std::chrono::seconds(5)));
    require(stream->signal_end(1U, k_frames));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!stream->status().production_complete) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("Scene production timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::array<std::size_t, 6> controls{0U, 2048U, 2600U, 3000U, 7000U, 8192U};
    (*output)->play();
    std::vector<float> result;
    std::size_t step = 0U;
    std::size_t control = 0U;
    bool short_read = false;
    while (!(*output)->status().ended) {
        const auto consumed = result.size() / 2U;
        if (control < controls.size() && consumed == controls.at(control)) {
            switch (control++) {
            case 0U:
                require((*output)->set_volume(0.875F));
                break;
            case 1U:
                require((*output)->set_hptf(hptf_low_boost(), 1));
                break;
            case 2U:
                require((*output)->set_hptf(hptf_design("Preamp: -3 dB\nFilter 1: ON PK Fc 1700 Hz Gain 6 dB Q 0.75\n"),
                                            2));
                break;
            case 3U:
                require((*output)->set_hptf(
                    hptf_design("Preamp: -6 dB\nFilter 1: ON HSC Fc 4200 Hz Gain -4 dB Q 0.75\n"), 3));
                break;
            case 4U:
                require((*output)->set_hptf(std::nullopt, 4));
                break;
            default:
                require((*output)->set_volume(0.5F));
                break;
            }
        }
        auto count = sizes[step++ % sizes.size()];
        if (control < controls.size()) {
            count = std::min(count, controls.at(control) - consumed);
        }
        std::vector<float> callback(count * 2U);
        const auto produced = capture->pull(callback);
        result.insert(result.end(), callback.begin(), callback.begin() + static_cast<std::ptrdiff_t>(produced * 2U));
        revisions.push_back((*output)->hptf_applied_revision());
        // Only the final media read may be short; EOS follows on a later callback.
        if (short_read && produced != 0U) {
            throw std::runtime_error("device media after a short pull");
        }
        short_read = short_read || produced != count;
    }
    if (result.size() != static_cast<std::size_t>(k_frames) * 2U) {
        throw std::runtime_error("device partition changed the media length");
    }
    return result;
}

bool test_scene_device_pull_partitioning() {
    std::vector<std::uint64_t> revisions;
    const std::array<std::size_t, 1> whole{512U};
    const auto reference = render_device_partition(whole, revisions);
    bool ok = check(revisions.back() == 4U, "device partition: bypass revision applied last");
    ok &= check(std::ranges::find(revisions, 2U) == revisions.end(), "device partition: replaced target never applies");
    ok &= check(std::ranges::any_of(reference, [](float x) { return std::abs(x) > 0.5F; }),
                "device partition: output exercises the peak guard");
    const std::array<std::size_t, 5> fragmented{1U, 7U, 127U, 511U, 1024U};
    const std::array<std::size_t, 1> large{4096U};
    const std::array<std::size_t, 2> odd{3U, 1021U};
    const std::array<std::size_t, 1> callback{960U};
    for (const auto sizes : {std::span<const std::size_t>{fragmented},
                             std::span<const std::size_t>{large},
                             std::span<const std::size_t>{odd},
                             std::span<const std::size_t>{callback}}) {
        revisions.clear();
        const auto output = render_device_partition(sizes, revisions);
        ok &= check(std::ranges::equal(output,
                                       reference,
                                       [](float a, float b) {
                                           return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
                                       }),
                    "device output is independent of callback partitioning");
        ok &= check(std::ranges::find(revisions, 2U) == revisions.end(), "device partition: revision 2 stays queued");
    }
    return ok;
}

bool test_idle_stream_destruction() {
    std::atomic<bool> valid{true};
    std::vector<std::thread> controllers;
    for (int controller = 0; controller < 4; ++controller) {
        controllers.emplace_back([&valid] {
            for (int iteration = 0; iteration < 128; ++iteration) {
                SceneStreamConfig config;
                config.renderer.renderer = mradm::RendererSelection::saf;
                config.renderer.output_layout = "0+2+0";
                auto stream = SceneStreamEngine::create(config);
                if (!stream) {
                    valid.store(false);
                    return;
                }
                std::this_thread::yield();
                // No epoch/work was submitted: destruction must wake the idle worker.
                stream->reset();
            }
        });
    }
    for (auto& controller : controllers) {
        controller.join();
    }
    return check(valid.load(), "idle streams can be created and destroyed concurrently");
}

int main() {
    try {
        bool ok = test_transparency_and_volume();
        ok &= test_linked_smooth_gain();
        ok &= test_short_end_and_reset();
        ok &= test_device_clock_and_epoch();
        ok &= test_unprotected_volume();
        ok &= test_real_pcm();
        ok &= test_scene_hptf_respects_ceiling();
        ok &= test_scene_hptf_unsupported_without_stereo_guard();
        ok &= test_scene_hptf_snapshot_on_epoch();
        ok &= test_scene_device_pull_partitioning();
        ok &= test_idle_stream_destruction();
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
