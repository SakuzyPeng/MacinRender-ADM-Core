// 22.2 LFE routing of the Apple backend (R3 F2 / F3, macOS only): through the C ABI (direct, split-power
// and the dual-LFE refusal) and its island table (the LFE side bus in offline and stream renders, a live override on
// it, and a channel whose blocks change from LFE to full range). The shared rules and the EAR / VBAP
// renders are Rust tests (mradm-render tests/routing.rs, mradm-engine tests/lfe_render.rs).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/c_api.h"

#include "../support/apple_island_driver.h"
#include "../support/direct_speakers_fixture.h"
#include "../support/rust_services_test.h"
#include "render_common.h"

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

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
// The object a channel-bed import synthesizes (mradm-scene).
constexpr const char* k_bed_object = "AO_CHANNEL_BED";

[[nodiscard]] std::filesystem::path unique_path(std::string_view stem) {
    static std::atomic<unsigned int> sequence{0};
    return std::filesystem::temp_directory_path() /
           (std::string{stem} + "_" + std::to_string(sequence.fetch_add(1)) + ".wav");
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
    auto writer = mradm::audio::WriterHandle::open(path.string(), static_cast<uint32_t>(signals.size()), k_sample_rate);
    if (!writer) {
        return false;
    }
    const auto samples = make_input_samples(signals);
    return writer->write(samples.data(), k_frames) == k_frames && writer->finish().has_value();
}

std::vector<double> channel_rms(const apple_test::Audio& audio) {
    std::vector<double> rms(audio.channels, 0.0);
    for (uint64_t frame = 0; frame < audio.frames; ++frame) {
        for (uint32_t channel = 0; channel < audio.channels; ++channel) {
            const double sample = audio.samples[(frame * audio.channels) + channel];
            rms[channel] += sample * sample;
        }
    }
    std::ranges::transform(
        rms, rms.begin(), [&](double sum) { return std::sqrt(sum / static_cast<double>(audio.frames)); });
    return rms;
}

struct RenderCapture {
    adm_error_code_t code{ADM_ERROR_OK};
    std::string message;
    std::vector<double> rms;
    bool output_exists{false};
};

// A C ABI Apple render of a labelled channel bed to 22.2.
RenderCapture render_case(const std::vector<std::string>& labels, const std::vector<SignalSpec>& signals, bool split) {
    const auto input = unique_path("lfe_routing_input");
    const auto output = unique_path("lfe_routing_output");
    const FileGuard input_guard(input);
    const FileGuard output_guard(output);
    RenderCapture capture;
    if (!write_input(input, signals)) {
        capture.code = ADM_ERROR_IO;
        return capture;
    }
    std::string csv;
    for (const auto& label : labels) {
        csv += (csv.empty() ? "" : ",") + label;
    }
    adm_context_t* context = adm_create_context();
    adm_render_options_t* options = adm_create_render_options();
    adm_render_options_set_renderer(options, ADM_RENDERER_APPLE);
    adm_render_options_set_output_layout(options, "22.2");
    adm_render_options_set_input_channel_labels(options, csv.c_str());
    adm_render_options_set_lfe_routing_mode(options, split ? ADM_LFE_ROUTING_SPLIT_POWER : ADM_LFE_ROUTING_DIRECT);
    adm_render_options_set_peak_limit(options, 0);
    adm_render_result_t* result = nullptr;
    capture.code = adm_render_file_ex(
        context, input.string().c_str(), output.string().c_str(), options, nullptr, nullptr, &result);
    capture.message = adm_render_result_message(result);
    adm_destroy_render_result(result);
    adm_destroy_render_options(options);
    adm_destroy_context(context);
    capture.output_exists = std::filesystem::exists(output);
    if (capture.code == ADM_ERROR_OK) {
        if (auto audio = apple_test::read_wav(output)) {
            capture.rms = channel_rms(*audio);
        }
    }
    return capture;
}

bool near_ratio(double value, double reference, double ratio, double tolerance = 0.025) {
    return reference > 1.0e-8 && std::abs((value / reference) - ratio) <= tolerance;
}

// The Apple backend's 22.2 LFE routing (restored in F3 after the shared EAR / VBAP helper moved to
// Rust): direct LFE1 / LFE2 on ch3 / ch9 only, split-power at sqrt(0.5) on both, a native dual LFE
// without crosstalk, the dual split refused before any output, and full-range routing unchanged.
bool verify_apple_backend() {
    constexpr SignalSpec k_lfe1{0.20F, 80.0F};
    constexpr SignalSpec k_lfe2{0.10F, 160.0F};
    constexpr double k_silence = 1.0e-7;
    const auto direct_lfe1 = render_case({"LFE1"}, {k_lfe1}, false);
    const auto direct_lfe2 = render_case({"LFE2"}, {k_lfe2}, false);
    const auto split_lfe1 = render_case({"LFE1"}, {k_lfe1}, true);
    const auto split_lfe2 = render_case({"LFE2"}, {k_lfe2}, true);
    const auto direct_dual = render_case({"LFE1", "LFE2"}, {k_lfe1, k_lfe2}, false);
    const auto split_dual = render_case({"LFE1", "LFE2"}, {k_lfe1, k_lfe2}, true);
    const auto full_range_direct = render_case({"M+000"}, {{0.12F, 1000.0F}}, false);
    const auto full_range_split = render_case({"M+000"}, {{0.12F, 1000.0F}}, true);

    const auto rendered = [](const RenderCapture& c) { return c.code == ADM_ERROR_OK && c.rms.size() == 24U; };
    bool ok = check(rendered(direct_lfe1) && rendered(direct_lfe2) && rendered(split_lfe1) && rendered(split_lfe2) &&
                        rendered(direct_dual) && rendered(full_range_direct) && rendered(full_range_split),
                    "Apple: the 22.2 LFE cases render 24 channels");
    if (!ok) {
        return false;
    }
    const auto only_lfe_slots_active = [&](const RenderCapture& c) {
        for (std::size_t channel = 0; channel < c.rms.size(); ++channel) {
            if (channel != 3U && channel != 9U && c.rms[channel] >= k_silence) {
                return false;
            }
        }
        return true;
    };
    ok = check(direct_lfe1.rms[3] > 1.0e-4 && direct_lfe1.rms[9] < k_silence, "Apple: direct LFE1 is on ch3 only");
    ok =
        check(direct_lfe2.rms[3] < k_silence && direct_lfe2.rms[9] > 1.0e-4, "Apple: direct LFE2 is on ch9 only") && ok;
    ok = check(only_lfe_slots_active(direct_lfe1) && only_lfe_slots_active(direct_lfe2) &&
                   only_lfe_slots_active(split_lfe1) && only_lfe_slots_active(split_lfe2) &&
                   only_lfe_slots_active(direct_dual),
               "Apple: LFE never leaks into any of the 22 full-range channels") &&
         ok;
    ok = check(near_ratio(split_lfe1.rms[3], direct_lfe1.rms[3], std::sqrt(0.5)) &&
                   near_ratio(split_lfe1.rms[9], direct_lfe1.rms[3], std::sqrt(0.5)) &&
                   std::abs(split_lfe1.rms[3] - split_lfe1.rms[9]) < 1.0e-6,
               "Apple: split LFE1 reaches ch3/ch9 equally at sqrt(0.5)") &&
         ok;
    ok = check(near_ratio(split_lfe2.rms[3], direct_lfe2.rms[9], std::sqrt(0.5)) &&
                   near_ratio(split_lfe2.rms[9], direct_lfe2.rms[9], std::sqrt(0.5)) &&
                   std::abs(split_lfe2.rms[3] - split_lfe2.rms[9]) < 1.0e-6,
               "Apple: split LFE2 reaches ch3/ch9 equally at sqrt(0.5)") &&
         ok;
    ok = check(near_ratio(direct_dual.rms[3], direct_lfe1.rms[3], 1.0) &&
                   near_ratio(direct_dual.rms[9], direct_lfe2.rms[9], 1.0),
               "Apple: native dual LFE stays independent without crosstalk") &&
         ok;
    ok = check(split_dual.code == ADM_ERROR_INVALID_ARGUMENT && !split_dual.output_exists &&
                   split_dual.message.find("both LFE1 and LFE2") != std::string::npos,
               "Apple: split dual LFE rejects before leaving an output file") &&
         ok;
    double full_range_energy = 0.0;
    double full_range_max_delta = 0.0;
    for (std::size_t channel = 0; channel < full_range_direct.rms.size(); ++channel) {
        full_range_energy += full_range_direct.rms[channel] * full_range_direct.rms[channel];
        full_range_max_delta =
            std::max(full_range_max_delta, std::abs(full_range_direct.rms[channel] - full_range_split.rms[channel]));
    }
    return check(full_range_energy > 1.0e-8 && full_range_max_delta < 1.0e-7,
                 "Apple: split-power leaves all non-LFE routing unchanged") &&
           ok;
}

// NOLINTNEXTLINE(readability-function-size) -- one end-to-end side-bus lifecycle scenario.
bool verify_apple_side_bus_stream_and_override() {
    constexpr std::size_t k_channels = 24U;
    const auto input = unique_path("apple_lfe_stream_input");
    const FileGuard input_guard(input);
    if (!check(write_input(input, {{0.2F, 80.0F}}), "Apple LFE stream: write input")) {
        return false;
    }
    apple_test::Settings settings;
    settings.input = input.string();
    settings.layout = "9+10+3";
    const apple_test::Prepared prepared(settings, {"LFE1"});
    if (!check(prepared.ok(), "Apple LFE stream: prepare pure side-bus scene")) {
        return false;
    }
    const auto output = unique_path("apple_lfe_stream_reference");
    const FileGuard output_guard(output);
    apple_test::Caller caller;
    if (!check(apple_test::render(prepared, settings, output, caller).ok(),
               "Apple LFE stream: offline reference render")) {
        return false;
    }
    const auto audio = apple_test::read_wav(output);
    if (!audio || audio->channels != k_channels || audio->frames != k_frames) {
        return check(false, "Apple LFE stream: reference is 24-channel and full-length");
    }
    const auto& reference = audio->samples;

    const auto neutral = apple_test::stream_all(prepared, settings, {1024U});
    apple_test::Stream edited_stream(prepared, settings);
    if (!neutral || !edited_stream.ok()) {
        return check(false, "Apple LFE stream: open realtime streams");
    }
    // The override lands after the first 1024 frames.
    std::vector<float> edited;
    std::vector<float> block(std::size_t{1024} * k_channels, 0.0F);
    for (bool applied = false;;) {
        std::ranges::fill(block, 0.0F);
        const auto produced = edited_stream.process(block, 1024U);
        if (!produced) {
            return check(false, "Apple LFE stream: process edited audio");
        }
        if (*produced == 0U) {
            break;
        }
        edited.insert(edited.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*produced * k_channels));
        if (!applied && edited.size() >= std::size_t{1024} * k_channels) {
            edited_stream.set_overrides({{k_bed_object, -20.0F, false, std::nullopt, "LFE1"}}, 1U);
            applied = true;
        }
    }
    if (neutral->size() != reference.size() || edited.size() != reference.size()) {
        return check(false, "Apple LFE stream: stream/offline frame counts match");
    }
    double max_diff = 0.0;
    for (std::size_t index = 0; index < reference.size(); ++index) {
        max_diff = std::max(max_diff, std::abs(static_cast<double>((*neutral)[index] - reference[index])));
    }
    bool ok = check(max_diff < 1.0e-7, "Apple LFE stream: pure side bus matches the offline render");

    constexpr std::size_t k_edit_frame = 1024U;
    if (edited.size() > (k_edit_frame * k_channels) + 9U) {
        ok = check(std::abs(edited[(k_edit_frame * k_channels) + 3U] - reference[(k_edit_frame * k_channels) + 3U]) <
                       1.0e-7F,
                   "Apple LFE stream: override ramp starts continuously at the old gain") &&
             ok;
    }
    constexpr std::size_t k_settled_begin = k_edit_frame + 1200U;
    constexpr std::size_t k_settled_end = 4400U;
    double reference_energy = 0.0;
    double edited_energy = 0.0;
    double lfe2_energy = 0.0;
    for (std::size_t frame = k_settled_begin; frame < k_settled_end && (frame * k_channels) + 9U < edited.size();
         ++frame) {
        const double ref = reference[(frame * k_channels) + 3U];
        const double changed = edited[(frame * k_channels) + 3U];
        reference_energy += ref * ref;
        edited_energy += changed * changed;
        const double lfe2 = edited[(frame * k_channels) + 9U];
        lfe2_energy += lfe2 * lfe2;
    }
    ok = check(reference_energy > 0.0 && edited_energy > reference_energy * 0.008 &&
                   edited_energy < reference_energy * 0.012,
               "Apple LFE stream: realtime override settles at -20 dB through the side bus") &&
         ok;
    ok = check(lfe2_energy < 1.0e-12, "Apple LFE stream: direct override does not leak into LFE2") && ok;

    // One source channel may change from LFE to a regular DirectSpeakers target over time. The
    // Apple prepare recipe must split the intervals, not classify the whole track as LFE and drop
    // its later full-range block.
    constexpr uint64_t k_switch_frame = 2048U;
    const auto mixed_input = unique_path("apple_lfe_mixed_blocks_input");
    const auto mixed_output = unique_path("apple_lfe_mixed_blocks");
    const FileGuard mixed_input_guard(mixed_input);
    const FileGuard mixed_output_guard(mixed_output);
    fixture::write_mono_direct_speakers(
        mixed_input,
        k_sample_rate,
        make_input_samples({{0.2F, 80.0F}}),
        1.0F,
        {fixture::direct_speakers_block("LFE1", 0U, k_switch_frame, k_sample_rate),
         fixture::direct_speakers_block("M+000", k_switch_frame, k_frames, k_sample_rate)});
    apple_test::Settings mixed_settings = settings;
    mixed_settings.input = mixed_input.string();
    const apple_test::Prepared mixed_prepared(mixed_settings);
    if (!check(mixed_prepared.ok(), "Apple mixed blocks: prepare") ||
        !check(apple_test::render(mixed_prepared, mixed_settings, mixed_output, caller).ok(),
               "Apple mixed blocks: offline render")) {
        return false;
    }
    const auto mixed = apple_test::read_wav(mixed_output);
    if (!mixed || mixed->channels != k_channels) {
        return check(false, "Apple mixed blocks: output is 24-channel");
    }
    double first_lfe_energy = 0.0;
    double later_lfe_energy = 0.0;
    double later_spatial_energy = 0.0;
    for (uint64_t frame = 0; frame < mixed->frames; ++frame) {
        const auto offset = static_cast<std::size_t>(frame) * k_channels;
        if (frame < k_switch_frame) {
            const double sample = mixed->samples[offset + 3U];
            first_lfe_energy += sample * sample;
        } else if (frame >= k_switch_frame + 512U) {
            const double lfe = mixed->samples[offset + 3U];
            later_lfe_energy += lfe * lfe;
            for (std::size_t channel = 0; channel < k_channels; ++channel) {
                if (channel != 3U && channel != 9U) {
                    const double sample = mixed->samples[offset + channel];
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

} // namespace

int main() {
    mradm_test::bind_rust_services();
    bool ok = verify_apple_backend();
    ok = verify_apple_side_bus_stream_and_override() && ok;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
