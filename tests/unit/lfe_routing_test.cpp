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

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/render.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

#include "render_common.h"
#include "test_portable.h"

namespace {

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

class CapturingLogSink final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view /*module*/, std::string_view message) override {
        if (level == mradm::LogLevel::warning) {
            warnings_.emplace_back(message);
        }
    }

    [[nodiscard]] bool has_warning(std::string_view needle) const {
        return std::ranges::any_of(warnings_,
                                   [needle](const auto& warning) { return warning.find(needle) != std::string::npos; });
    }

  private:
    std::vector<std::string> warnings_;
};

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void append_lfe_block(mradm::RenderPlan& plan, mradm::SceneDirectSpeakersBlock block) {
    if (plan.scene.objects.empty()) {
        plan.scene.objects.emplace_back();
        plan.scene.objects.back().tracks.emplace_back();
    }
    plan.scene.objects.back().tracks.back().ds_blocks.push_back(std::move(block));
}

bool verify_shared_semantics() {
    using mradm::render_common::direct_speakers_lfe_target;
    using mradm::render_common::LfeTarget;

    bool ok = check(mradm::RenderOptions{}.lfe_routing_mode == mradm::LfeRoutingMode::direct,
                    "RenderOptions defaults to direct LFE routing");
    ok = check(mradm::RenderPlan{}.lfe_routing_mode == mradm::LfeRoutingMode::direct,
               "RenderPlan defaults to direct LFE routing") &&
         ok;

    constexpr std::array<std::string_view, 8> k_lfe1_aliases{
        "LFE", "LFE1", "LFEL", "RC_LFE", "RCLFE", "SUB", "Subwoofer", "low_frequency"};
    for (const auto alias : k_lfe1_aliases) {
        mradm::SceneDirectSpeakersBlock block;
        block.speaker_labels = {std::string{alias}};
        ok = check(direct_speakers_lfe_target(block) == LfeTarget::lfe1,
                   std::string{"LFE1 alias classified: "} + std::string{alias}) &&
             ok;
    }
    for (const auto alias : {std::string_view{"LFE2"}, std::string_view{"LFER"}}) {
        mradm::SceneDirectSpeakersBlock block;
        block.speaker_labels = {std::string{alias}};
        ok = check(direct_speakers_lfe_target(block) == LfeTarget::lfe2,
                   std::string{"LFE2 alias classified: "} + std::string{alias}) &&
             ok;
    }
    mradm::SceneDirectSpeakersBlock layered_aliases;
    layered_aliases.speaker_labels = {"LFE", "LFE2"};
    ok = check(direct_speakers_lfe_target(layered_aliases) == LfeTarget::lfe2,
               "explicit LFE2 wins over a generic LFE alias on the same metadata block") &&
         ok;
    mradm::SceneDirectSpeakersBlock low_pass_only;
    low_pass_only.low_pass_hz = 120.0F;
    ok = check(direct_speakers_lfe_target(low_pass_only) == LfeTarget::lfe1, "lowPass-only block classifies as LFE1") &&
         ok;
    mradm::SceneDirectSpeakersBlock non_lfe;
    non_lfe.speaker_labels = {"M+030"};
    ok = check(direct_speakers_lfe_target(non_lfe) == LfeTarget::none, "non-LFE block remains unclassified") && ok;

    mradm::RenderPlan repeated;
    repeated.output_layout = "9+10+3";
    repeated.lfe_routing_mode = mradm::LfeRoutingMode::split_power;
    for (int i = 0; i < 2; ++i) {
        mradm::SceneDirectSpeakersBlock block;
        block.speaker_labels = {"LFE1"};
        append_lfe_block(repeated, std::move(block));
    }
    CapturingLogSink repeated_logs;
    const auto repeated_result = mradm::render_common::resolve_lfe_routing(repeated, repeated_logs, "test");
    ok = check(repeated_result.has_value() && repeated_result->has_lfe1 && !repeated_result->has_lfe2,
               "repeated LFE1 remains one semantic LFE") &&
         ok;
    mradm::RenderPlan direct = repeated;
    direct.lfe_routing_mode = mradm::LfeRoutingMode::direct;
    CapturingLogSink direct_logs;
    const auto direct_result = mradm::render_common::resolve_lfe_routing(direct, direct_logs, "test");
    ok = check(direct_result.has_value() && direct_result->gain(LfeTarget::lfe1, LfeTarget::lfe1) == 1.0F &&
                   direct_result->gain(LfeTarget::lfe1, LfeTarget::lfe2) == 0.0F,
               "direct routing uses an exact unity coefficient without crossfeed") &&
         ok;

    mradm::RenderPlan lfe2_only;
    lfe2_only.output_layout = "9+10+3";
    lfe2_only.lfe_routing_mode = mradm::LfeRoutingMode::split_power;
    mradm::SceneDirectSpeakersBlock lfe2_block;
    lfe2_block.speaker_labels = {"LFER"};
    append_lfe_block(lfe2_only, std::move(lfe2_block));
    CapturingLogSink lfe2_logs;
    const auto lfe2_result = mradm::render_common::resolve_lfe_routing(lfe2_only, lfe2_logs, "test");
    ok = check(lfe2_result.has_value() && !lfe2_result->has_lfe1 && lfe2_result->has_lfe2,
               "standalone LFE2 is valid for split-power") &&
         ok;

    mradm::RenderPlan dual = repeated;
    mradm::SceneDirectSpeakersBlock second;
    second.speaker_labels = {"LFE2"};
    append_lfe_block(dual, std::move(second));
    CapturingLogSink dual_logs;
    const auto dual_result = mradm::render_common::resolve_lfe_routing(dual, dual_logs, "test");
    ok = check(!dual_result && dual_result.error().code == mradm::ErrorCode::invalid_argument &&
                   dual_result.error().message.find("both LFE1 and LFE2") != std::string::npos,
               "dual semantic LFE split is rejected with the stable user error") &&
         ok;

    mradm::RenderPlan empty;
    empty.output_layout = "9+10+3";
    empty.lfe_routing_mode = mradm::LfeRoutingMode::split_power;
    CapturingLogSink empty_logs;
    const auto empty_result = mradm::render_common::resolve_lfe_routing(empty, empty_logs, "test");
    ok =
        check(empty_result.has_value() && empty_logs.has_warning("no semantic LFE"), "split-power without LFE warns") &&
        ok;

    mradm::RenderPlan other_layout = lfe2_only;
    other_layout.output_layout = "0+5+0";
    CapturingLogSink other_logs;
    const auto other_result = mradm::render_common::resolve_lfe_routing(other_layout, other_logs, "test");
    ok = check(other_result.has_value() && !other_result->applies_to_22_2 &&
                   other_logs.has_warning("only applies to 22.2"),
               "split-power on another layout warns and does not apply") &&
         ok;

    if (repeated_result) {
        const float split = repeated_result->gain(LfeTarget::lfe1, LfeTarget::lfe2);
        ok = check(std::abs(split - std::sqrt(0.5F)) < 1.0e-7F, "split-power coefficient is sqrt(0.5)") && ok;
    }
    return ok;
}

struct SignalSpec {
    float amplitude{0.0F};
    float frequency{80.0F};
};

[[nodiscard]] std::filesystem::path unique_path(std::string_view stem) {
    static std::atomic<unsigned int> sequence{0};
    return std::filesystem::path{mr_test::temp_prefix() + std::string{stem} + "_" +
                                 std::to_string(sequence.fetch_add(1)) + ".wav"};
}

bool write_input(const std::filesystem::path& path, const std::vector<SignalSpec>& signals) {
    constexpr uint32_t k_sample_rate = 48000U;
    constexpr uint64_t k_frames = 4800U;
    auto writer =
        mradm::audio::FloatWavWriter::open(path.string(), static_cast<uint32_t>(signals.size()), k_sample_rate);
    if (!writer) {
        return false;
    }
    std::vector<float> samples(k_frames * signals.size(), 0.0F);
    for (uint64_t frame = 0; frame < k_frames; ++frame) {
        for (std::size_t channel = 0; channel < signals.size(); ++channel) {
            const auto& signal = signals[channel];
            const float phase = 2.0F * std::numbers::pi_v<float> * signal.frequency * static_cast<float>(frame) /
                                static_cast<float>(k_sample_rate);
            samples[(frame * signals.size()) + channel] = signal.amplitude * std::sin(phase);
        }
    }
    return writer->write(samples.data(), k_frames) == k_frames;
}

struct RenderCapture {
    mradm::Error error;
    std::vector<double> rms;
    bool output_exists{false};
};

RenderCapture render_case(mradm::RendererSelection renderer,
                          const std::vector<std::string>& labels,
                          const std::vector<SignalSpec>& signals,
                          mradm::LfeRoutingMode mode,
                          std::string_view output_layout = "22.2",
                          CapturingLogSink* capture_logs = nullptr) {
    const auto input = unique_path("lfe_routing_input");
    const auto output = unique_path("lfe_routing_output");
    FileGuard input_guard(input);
    FileGuard output_guard(output);
    if (!write_input(input, signals)) {
        return {{mradm::ErrorCode::io_error, "failed to write test input", {}}, {}, false};
    }

    mradm::RenderRequest request;
    request.input_path = input;
    request.output_path = output;
    request.options.renderer = renderer;
    request.options.output_layout = std::string{output_layout};
    request.options.input_channel_labels = labels;
    request.options.lfe_routing_mode = mode;
    request.options.peak_limit = false;
    request.options.measure_loudness = false;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink null_logs;
    mradm::LogSink& logs =
        capture_logs != nullptr ? static_cast<mradm::LogSink&>(*capture_logs) : static_cast<mradm::LogSink&>(null_logs);
    const auto result = service.render(request, progress, logs);

    RenderCapture capture;
    capture.error = result.error;
    capture.output_exists = std::filesystem::exists(output);
    if (!result.success()) {
        return capture;
    }
    auto reader = mradm::audio::FloatWavReader::open(output.string());
    if (!reader) {
        capture.error = reader.error();
        return capture;
    }
    std::vector<float> samples(reader->frame_count() * reader->channels(), 0.0F);
    if (reader->read(samples.data(), reader->frame_count()) != reader->frame_count()) {
        capture.error = {mradm::ErrorCode::io_error, "short read of rendered test output", {}};
        return capture;
    }
    capture.rms.assign(reader->channels(), 0.0);
    for (uint64_t frame = 0; frame < reader->frame_count(); ++frame) {
        for (uint32_t channel = 0; channel < reader->channels(); ++channel) {
            const double sample = samples[(frame * reader->channels()) + channel];
            capture.rms[channel] += sample * sample;
        }
    }
    std::ranges::transform(capture.rms, capture.rms.begin(), [frames = reader->frame_count()](double sum) {
        return std::sqrt(sum / static_cast<double>(frames));
    });
    return capture;
}

bool near_ratio(double value, double reference, double ratio, double tolerance = 0.025) {
    return reference > 1.0e-8 && std::abs((value / reference) - ratio) <= tolerance;
}

bool verify_backend(mradm::RendererSelection renderer, std::string_view name) {
    constexpr SignalSpec k_lfe1{0.20F, 80.0F};
    constexpr SignalSpec k_lfe2{0.10F, 160.0F};
    constexpr double k_silence = 1.0e-7;
    const auto direct_lfe1 = render_case(renderer, {"LFE1"}, {k_lfe1}, mradm::LfeRoutingMode::direct);
    const auto direct_lfe2 = render_case(renderer, {"LFE2"}, {k_lfe2}, mradm::LfeRoutingMode::direct);
    const auto split_lfe1 = render_case(renderer, {"LFE1"}, {k_lfe1}, mradm::LfeRoutingMode::split_power);
    const auto split_lfe2 = render_case(renderer, {"LFE2"}, {k_lfe2}, mradm::LfeRoutingMode::split_power);
    const auto direct_dual = render_case(renderer, {"LFE1", "LFE2"}, {k_lfe1, k_lfe2}, mradm::LfeRoutingMode::direct);
    const auto split_dual =
        render_case(renderer, {"LFE1", "LFE2"}, {k_lfe1, k_lfe2}, mradm::LfeRoutingMode::split_power);
    const auto full_range_direct = render_case(renderer, {"M+000"}, {{0.12F, 1000.0F}}, mradm::LfeRoutingMode::direct);
    const auto full_range_split =
        render_case(renderer, {"M+000"}, {{0.12F, 1000.0F}}, mradm::LfeRoutingMode::split_power);

    const auto all_success = [](const RenderCapture& capture) {
        return capture.error.ok() && capture.rms.size() == 24U;
    };
    bool ok = check(all_success(direct_lfe1), std::string{name} + ": direct LFE1 renders 24 channels") &&
              check(all_success(direct_lfe2), std::string{name} + ": direct LFE2 renders 24 channels") &&
              check(all_success(split_lfe1), std::string{name} + ": split LFE1 renders 24 channels") &&
              check(all_success(split_lfe2), std::string{name} + ": split LFE2 renders 24 channels") &&
              check(all_success(direct_dual), std::string{name} + ": native dual LFE direct renders") &&
              check(all_success(full_range_direct) && all_success(full_range_split),
                    std::string{name} + ": full-range reference renders in both modes");
    if (!ok) {
        return false;
    }

    const auto only_lfe_slots_active = [&](const RenderCapture& capture) {
        for (std::size_t channel = 0; channel < capture.rms.size(); ++channel) {
            if (channel != 3U && channel != 9U && capture.rms[channel] >= k_silence) {
                return false;
            }
        }
        return true;
    };

    ok = check(direct_lfe1.rms[3] > 1.0e-4 && direct_lfe1.rms[9] < k_silence,
               std::string{name} + ": direct LFE1 is unity on ch3 only");
    ok = check(direct_lfe2.rms[3] < k_silence && direct_lfe2.rms[9] > 1.0e-4,
               std::string{name} + ": direct LFE2 is unity on ch9 only") &&
         ok;
    ok = check(only_lfe_slots_active(direct_lfe1) && only_lfe_slots_active(direct_lfe2) &&
                   only_lfe_slots_active(split_lfe1) && only_lfe_slots_active(split_lfe2) &&
                   only_lfe_slots_active(direct_dual),
               std::string{name} + ": LFE never leaks into any of the 22 full-range channels") &&
         ok;
    ok = check(near_ratio(split_lfe1.rms[3], direct_lfe1.rms[3], std::sqrt(0.5)) &&
                   near_ratio(split_lfe1.rms[9], direct_lfe1.rms[3], std::sqrt(0.5)) &&
                   std::abs(split_lfe1.rms[3] - split_lfe1.rms[9]) < 1.0e-6,
               std::string{name} + ": split LFE1 reaches ch3/ch9 equally at sqrt(0.5)") &&
         ok;
    ok = check(near_ratio(split_lfe2.rms[3], direct_lfe2.rms[9], std::sqrt(0.5)) &&
                   near_ratio(split_lfe2.rms[9], direct_lfe2.rms[9], std::sqrt(0.5)) &&
                   std::abs(split_lfe2.rms[3] - split_lfe2.rms[9]) < 1.0e-6,
               std::string{name} + ": split LFE2 reaches ch3/ch9 equally at sqrt(0.5)") &&
         ok;
    ok = check(near_ratio(direct_dual.rms[3], direct_lfe1.rms[3], 1.0) &&
                   near_ratio(direct_dual.rms[9], direct_lfe2.rms[9], 1.0),
               std::string{name} + ": native dual LFE stays independent without crosstalk") &&
         ok;
    ok = check(split_dual.error.code == mradm::ErrorCode::invalid_argument && !split_dual.output_exists &&
                   split_dual.error.message.find("both LFE1 and LFE2") != std::string::npos,
               std::string{name} + ": split dual LFE rejects before leaving an output file") &&
         ok;
    double full_range_energy = 0.0;
    double full_range_max_delta = 0.0;
    for (std::size_t channel = 0; channel < full_range_direct.rms.size(); ++channel) {
        full_range_energy += full_range_direct.rms[channel] * full_range_direct.rms[channel];
        full_range_max_delta =
            std::max(full_range_max_delta, std::abs(full_range_direct.rms[channel] - full_range_split.rms[channel]));
    }
    ok = check(full_range_energy > 1.0e-8 && full_range_max_delta < 1.0e-7,
               std::string{name} + ": split-power leaves all non-LFE routing unchanged") &&
         ok;
    return ok;
}

bool verify_non_22_2_warning() {
    CapturingLogSink logs;
    const auto result = render_case(
        mradm::RendererSelection::ear, {"LFE1"}, {{0.2F, 80.0F}}, mradm::LfeRoutingMode::split_power, "5.1", &logs);
    return check(result.error.ok() && result.rms.size() == 6U && logs.has_warning("only applies to 22.2"),
                 "split-power on a non-22.2 layout preserves rendering and logs a warning");
}

#ifdef __APPLE__
// NOLINTNEXTLINE(readability-function-size) -- one end-to-end side-bus lifecycle scenario.
bool verify_apple_side_bus_stream_and_override() {
    const auto input = unique_path("apple_lfe_stream_input");
    const auto output = unique_path("apple_lfe_stream_reference");
    FileGuard input_guard(input);
    FileGuard output_guard(output);
    if (!check(write_input(input, {{0.2F, 80.0F}}), "Apple LFE stream: write input")) {
        return false;
    }

    mradm::io::SceneImportOptions import_options;
    import_options.input_channel_labels = {"LFE1"};
    auto scene = mradm::io::import_scene(input.string(), import_options);
    if (!check(scene.has_value(), "Apple LFE stream: import channel-bed scene")) {
        return false;
    }

    mradm::RenderPlan plan;
    plan.input_path = input.string();
    plan.output_path = output.string();
    plan.output_layout = "9+10+3";
    plan.scene = *scene;
    plan.lfe_routing_mode = mradm::LfeRoutingMode::direct;

    auto renderer = mradm::create_apple_renderer();
    mradm::NullLogSink logs;
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
                object_override.object_id = scene->objects.front().id;
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
    const auto mixed_output = unique_path("apple_lfe_mixed_blocks");
    FileGuard mixed_output_guard(mixed_output);
    mradm::RenderPlan mixed_plan = plan;
    mixed_plan.output_path = mixed_output.string();
    auto& mixed_blocks = mixed_plan.scene.objects.front().tracks.front().ds_blocks;
    mixed_blocks.front().end_sample = k_switch_frame;
    auto spatial_block = mixed_blocks.front();
    spatial_block.speaker_labels = {"M+000"};
    spatial_block.low_pass_hz.reset();
    spatial_block.has_position = true;
    spatial_block.azimuth = 0.0F;
    spatial_block.elevation = 0.0F;
    spatial_block.distance = 1.0F;
    spatial_block.start_sample = k_switch_frame;
    spatial_block.end_sample = mixed_plan.scene.info.num_frames;
    mixed_blocks.push_back(std::move(spatial_block));

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

int main() {
    bool ok = verify_shared_semantics();
    ok = verify_backend(mradm::RendererSelection::ear, "EAR") && ok;
    ok = verify_backend(mradm::RendererSelection::saf, "SAF VBAP") && ok;
#ifdef __APPLE__
    ok = verify_backend(mradm::RendererSelection::apple, "Apple") && ok;
    ok = verify_apple_side_bus_stream_and_override() && ok;
#endif
    ok = verify_non_22_2_warning() && ok;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
