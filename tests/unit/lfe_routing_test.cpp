#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "render_common.h"
#ifdef __APPLE__
#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_apple.h"
#include "adm/render_service.h"

#include "../support/apple_renderer/apple_renderer.h"
#include "../support/direct_speakers_fixture.h"
#include "adm_scene.h"
#include "render_common_scene.h"
#include "test_portable.h"
#endif

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

#ifdef __APPLE__
class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

  private:
    std::filesystem::path path_;
};

struct SignalSpec {
    float amplitude{0.0F};
    float frequency{80.0F};
};

constexpr uint32_t k_sample_rate = 48000U;
constexpr uint64_t k_frames = 4800U;

[[nodiscard]] std::filesystem::path unique_path(std::string_view stem) {
    static std::atomic<unsigned int> sequence{0};
    return std::filesystem::path{mr_test::temp_prefix() + std::string{stem} + "_" +
                                 std::to_string(sequence.fetch_add(1)) + ".wav"};
}

std::vector<float> make_input_samples(const std::vector<SignalSpec>& signals) {
    std::vector<float> samples(k_frames * signals.size(), 0.0F);
    for (uint64_t frame = 0; frame < k_frames; ++frame) {
        for (std::size_t channel = 0; channel < signals.size(); ++channel) {
            const auto& signal = signals[channel];
            const float phase = 2.0F * std::numbers::pi_v<float> * signal.frequency * static_cast<float>(frame) /
                                static_cast<float>(k_sample_rate);
            samples[(frame * signals.size()) + channel] = signal.amplitude * std::sin(phase);
        }
    }
    return samples;
}

bool write_input(const std::filesystem::path& path, const std::vector<SignalSpec>& signals) {
    auto writer =
        mradm::audio::FloatWavWriter::open(path.string(), static_cast<uint32_t>(signals.size()), k_sample_rate);
    if (!writer) {
        return false;
    }
    const auto samples = make_input_samples(signals);
    return writer->write(samples.data(), k_frames) == k_frames;
}

// NOLINTNEXTLINE(readability-function-size) -- one end-to-end side-bus lifecycle scenario.
bool verify_apple_side_bus_stream_and_override() {
    const auto input = unique_path("apple_lfe_stream_input");
    const auto output = unique_path("apple_lfe_stream_reference");
    FileGuard input_guard(input);
    FileGuard output_guard(output);
    if (!check(write_input(input, {{0.2F, 80.0F}}), "Apple LFE stream: write input")) {
        return false;
    }

    mradm::RenderOptions import_options;
    import_options.input_channel_labels = {"LFE1"};
    mradm::NullLogSink logs;
    auto scene = mradm::RenderService{}.prepare_preview_scene(input, import_options, logs);
    if (!check(scene.has_value(), "Apple LFE stream: import channel-bed scene")) {
        return false;
    }

    mradm::ScenePlan plan;
    plan.input_path = input.string();
    plan.output_path = output.string();
    plan.output_layout = "9+10+3";
    plan.scene = mradm::adm_scene(*scene);
    plan.scene_handle = scene->handle;
    plan.lfe_routing_mode = mradm::LfeRoutingMode::direct;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "Apple LFE stream: prepare pure side-bus scene") ||
        !check(renderer->render_window(**prepared, plan, progress, logs).has_value(),
               "Apple LFE stream: offline reference render")) {
        return false;
    }

    auto reader = mradm::audio::FloatWavReader::open(output.string());
    if (!check(reader.has_value() && reader->channels() == 24U, "Apple LFE stream: reference is 24-channel")) {
        return false;
    }
    std::vector<float> reference(reader->frame_count() * reader->channels(), 0.0F);
    reader->read(reference.data(), reader->frame_count());

    const auto pull_all = [&](mradm::IRenderStream& stream, bool apply_override) {
        std::vector<float> rendered;
        std::vector<float> block(std::size_t{1024} * 24U, 0.0F);
        std::size_t total_frames = 0;
        bool override_applied = false;
        for (;;) {
            std::ranges::fill(block, 0.0F);
            auto produced = stream.process(std::span<float>(block), 1024U);
            if (!produced || *produced == 0U) {
                break;
            }
            rendered.insert(
                rendered.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*produced * 24U));
            total_frames += *produced;
            if (apply_override && !override_applied && total_frames >= 1024U) {
                mradm::LiveObjectOverride object_override;
                object_override.object_id = plan.scene.objects.front().id;
                object_override.speaker_label = "LFE1";
                object_override.gain_db = -20.0F;
                mradm::LiveOverrides overrides;
                overrides.revision = 1U;
                overrides.objects.push_back(std::move(object_override));
                stream.set_overrides(overrides);
                override_applied = true;
            }
        }
        return rendered;
    };

    auto neutral_stream = renderer->open_stream(**prepared, plan, logs);
    auto edited_stream = renderer->open_stream(**prepared, plan, logs);
    if (!check(neutral_stream.has_value() && edited_stream.has_value(), "Apple LFE stream: open realtime streams")) {
        return false;
    }
    const auto neutral = pull_all(**neutral_stream, false);
    const auto edited = pull_all(**edited_stream, true);
    bool ok = check(neutral.size() == reference.size() && edited.size() == reference.size(),
                    "Apple LFE stream: stream/offline frame counts match");
    double max_diff = 0.0;
    for (std::size_t index = 0; index < std::min(neutral.size(), reference.size()); ++index) {
        max_diff = std::max(max_diff, std::abs(static_cast<double>(neutral[index] - reference[index])));
    }
    ok = check(max_diff < 1.0e-7, "Apple LFE stream: pure side bus matches offline render_window") && ok;

    constexpr std::size_t k_edit_frame = 1024U;
    if (edited.size() > (k_edit_frame * 24U) + 9U) {
        ok = check(std::abs(edited[(k_edit_frame * 24U) + 3U] - reference[(k_edit_frame * 24U) + 3U]) < 1.0e-7F,
                   "Apple LFE stream: override ramp starts continuously at the old gain") &&
             ok;
    }
    constexpr std::size_t k_settled_begin = k_edit_frame + 1200U;
    constexpr std::size_t k_settled_end = 4400U;
    double reference_energy = 0.0;
    double edited_energy = 0.0;
    double lfe2_energy = 0.0;
    for (std::size_t frame = k_settled_begin; frame < k_settled_end; ++frame) {
        const double ref = reference[(frame * 24U) + 3U];
        const double changed = edited[(frame * 24U) + 3U];
        reference_energy += ref * ref;
        edited_energy += changed * changed;
        const double lfe2 = edited[(frame * 24U) + 9U];
        lfe2_energy += lfe2 * lfe2;
    }
    ok = check(reference_energy > 0.0 && edited_energy > reference_energy * 0.008 &&
                   edited_energy < reference_energy * 0.012,
               "Apple LFE stream: realtime override settles at -20 dB through the side bus") &&
         ok;
    ok = check(lfe2_energy < 1.0e-12, "Apple LFE stream: direct override does not leak into LFE2") && ok;

    // One source channel may change from LFE to a regular DirectSpeakers target over
    // time. The Apple prepare recipe must split the intervals, not classify the whole
    // track as LFE and drop its later full-range block.
    constexpr uint64_t k_switch_frame = 2048U;
    const auto mixed_input = unique_path("apple_lfe_mixed_blocks_input");
    const auto mixed_output = unique_path("apple_lfe_mixed_blocks");
    FileGuard mixed_input_guard(mixed_input);
    FileGuard mixed_output_guard(mixed_output);
    fixture::write_mono_direct_speakers(
        mixed_input,
        k_sample_rate,
        make_input_samples({{0.2F, 80.0F}}),
        1.0F,
        {fixture::direct_speakers_block("LFE1", 0U, k_switch_frame, k_sample_rate),
         fixture::direct_speakers_block("M+000", k_switch_frame, k_frames, k_sample_rate)});
    auto mixed_scene = mradm::RenderService{}.prepare_preview_scene(mixed_input, {}, logs);
    if (!check(mixed_scene.has_value(), "Apple mixed blocks: import ADM fixture")) {
        return false;
    }
    mradm::ScenePlan mixed_plan = plan;
    mixed_plan.input_path = mixed_input.string();
    mixed_plan.output_path = mixed_output.string();
    mixed_plan.scene = mradm::adm_scene(*mixed_scene);
    mixed_plan.scene_handle = mixed_scene->handle;

    auto mixed_prepared = renderer->prepare(mixed_plan, logs);
    if (!check(mixed_prepared.has_value(), "Apple mixed blocks: prepare") ||
        !check(renderer->render_window(**mixed_prepared, mixed_plan, progress, logs).has_value(),
               "Apple mixed blocks: offline render")) {
        return false;
    }
    auto mixed_reader = mradm::audio::FloatWavReader::open(mixed_output.string());
    if (!check(mixed_reader.has_value() && mixed_reader->channels() == 24U,
               "Apple mixed blocks: output is 24-channel")) {
        return false;
    }
    std::vector<float> mixed_samples(mixed_reader->frame_count() * mixed_reader->channels(), 0.0F);
    mixed_reader->read(mixed_samples.data(), mixed_reader->frame_count());
    double first_lfe_energy = 0.0;
    double later_lfe_energy = 0.0;
    double later_spatial_energy = 0.0;
    for (uint64_t frame = 0; frame < mixed_reader->frame_count(); ++frame) {
        const auto offset = static_cast<std::size_t>(frame) * 24U;
        if (frame < k_switch_frame) {
            const double sample = mixed_samples[offset + 3U];
            first_lfe_energy += sample * sample;
        } else if (frame >= k_switch_frame + 512U) {
            const double lfe = mixed_samples[offset + 3U];
            later_lfe_energy += lfe * lfe;
            for (std::size_t channel = 0; channel < 24U; ++channel) {
                if (channel != 3U && channel != 9U) {
                    const double sample = mixed_samples[offset + channel];
                    later_spatial_energy += sample * sample;
                }
            }
        }
    }
    ok = check(first_lfe_energy > 1.0e-4 && later_lfe_energy < 1.0e-12 && later_spatial_energy > 1.0e-4,
               "Apple mixed blocks: LFE interval uses side bus and later non-LFE interval remains spatial") &&
         ok;
    return ok;
}
#endif

} // namespace

// The Windows system-spatial sink folds a split-power 22.2 LFE pair back to unity (moved here from
// scene_output_c_api_test, R3 E3).
bool verify_lfe_pair_recombination() {
    const float half = mradm::render_common::k_lfe_split_power_gain;
    return check(std::abs(mradm::render_common::mix_lfe_pair(half, half, true) - 1.0F) < 1.0e-6F &&
                     mradm::render_common::mix_lfe_pair(1.0F, 0.0F, false) == 1.0F,
                 "dual-LFE recombination keeps unity gain");
}

int main() {
    bool ok = verify_lfe_pair_recombination();
#ifdef __APPLE__
    ok = verify_apple_side_bus_stream_and_override() && ok;
#endif
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
