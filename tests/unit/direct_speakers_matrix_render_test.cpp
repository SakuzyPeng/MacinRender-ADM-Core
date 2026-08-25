#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/direct_speakers_matrix.h"
#include "adm/render.h"
#include "adm/render_ear.h"
#include "adm/render_vbap.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

namespace {

constexpr uint32_t k_sample_rate = 48000U;
constexpr uint64_t k_frames = 12317U;
constexpr uint64_t k_first_boundary = 4099U;
constexpr uint64_t k_second_boundary = 8203U;
constexpr uint32_t k_5_1_channels = 6U;
constexpr float k_object_gain = 0.8F;

constexpr std::string_view k_dynamic_matrix_json = R"json({
  "schema":"mradm.direct-speakers-matrix.v1",
  "output_layout":"5.1",
  "routes":[
    {
      "source_label":"C",
      "targets":[
        {"label":"L","weight":1},
        {"label":"R","weight":3}
      ]
    },
    {"source_label":"U+000","targets":[{"label":"M+000","weight":1}]},
    {"source_label":"M+110","mute":true}
  ]
})json";

constexpr std::string_view k_lfe_matrix_json = R"json({
  "schema":"mradm.direct-speakers-matrix.v1",
  "output_layout":"22.2",
  "routes":[{"source_label":"C","mute":true}]
})json";

bool check(bool condition, std::string_view message) {
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
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

  private:
    std::filesystem::path path_;
};

std::filesystem::path temp_path(std::string_view stem) {
    static std::atomic<unsigned int> path_sequence{0U};
    const auto sequence = path_sequence.fetch_add(1U);
    return std::filesystem::temp_directory_path() / (std::string{stem} + "-" + std::to_string(sequence) + ".wav");
}

std::vector<float> make_input_samples() {
    std::vector<float> samples(k_frames);
    for (uint64_t frame = 0; frame < k_frames; ++frame) {
        const float phase =
            2.0F * std::numbers::pi_v<float> * 440.0F * static_cast<float>(frame) / static_cast<float>(k_sample_rate);
        samples[frame] = 0.2F * std::sin(phase);
    }
    return samples;
}

bool write_input(const std::filesystem::path& path, const std::vector<float>& samples) {
    auto writer = mradm::audio::FloatWavWriter::open(path.string(), 1U, k_sample_rate);
    if (!writer) {
        std::cerr << "FAIL: cannot create matrix input: " << writer.error().message << '\n';
        return false;
    }
    return check(writer->write(samples.data(), samples.size()) == samples.size(), "matrix input writes every frame");
}

mradm::AdmScene make_dynamic_scene() {
    mradm::AdmScene scene;
    scene.info.sample_rate = k_sample_rate;
    scene.info.num_channels = 1U;
    scene.info.num_frames = k_frames;
    scene.info.source_kind = mradm::SceneSourceKind::channel_bed;

    mradm::SceneObject object;
    object.id = "AO_matrix_dynamic";
    object.gain = k_object_gain;

    mradm::SceneTrackRef track;
    track.channel_index = 0U;
    track.track_uid = "ATU_matrix_dynamic";

    mradm::SceneDirectSpeakersBlock split;
    split.speaker_labels = {"M+000"};
    split.gain = 0.5F;
    split.start_sample = 0U;
    split.end_sample = k_first_boundary;

    mradm::SceneDirectSpeakersBlock centre = split;
    centre.speaker_labels = {"U+000"};
    centre.gain = 0.25F;
    centre.start_sample = k_first_boundary;
    centre.end_sample = k_second_boundary;

    mradm::SceneDirectSpeakersBlock muted = split;
    muted.speaker_labels = {"M+110"};
    muted.gain = 0.75F;
    muted.start_sample = k_second_boundary;
    muted.end_sample = k_frames;

    track.ds_blocks = {split, centre, muted};
    object.tracks.push_back(std::move(track));
    scene.objects.push_back(std::move(object));
    return scene;
}

mradm::AdmScene make_lfe_scene() {
    mradm::AdmScene scene;
    scene.info.sample_rate = k_sample_rate;
    scene.info.num_channels = 1U;
    scene.info.num_frames = k_frames;
    scene.info.source_kind = mradm::SceneSourceKind::channel_bed;

    mradm::SceneObject object;
    object.id = "AO_matrix_lfe";
    object.gain = 0.7F;

    mradm::SceneTrackRef track;
    track.channel_index = 0U;
    track.track_uid = "ATU_matrix_lfe";
    mradm::SceneDirectSpeakersBlock lfe;
    lfe.speaker_labels = {"RC_LFE"};
    lfe.low_pass_hz = 120.0F;
    lfe.gain = 0.6F;
    lfe.start_sample = 0U;
    lfe.end_sample = k_frames;
    track.ds_blocks.push_back(std::move(lfe));
    object.tracks.push_back(std::move(track));
    scene.objects.push_back(std::move(object));
    return scene;
}

std::shared_ptr<const mradm::DirectSpeakersMatrix> parse_profile(std::string_view json) {
    auto parsed = mradm::parse_direct_speakers_matrix(json);
    if (!parsed) {
        std::cerr << "FAIL: matrix profile parse failed: " << parsed.error().message << '\n';
        return nullptr;
    }
    return std::make_shared<const mradm::DirectSpeakersMatrix>(std::move(*parsed));
}

std::optional<std::vector<float>> read_output(const std::filesystem::path& path, uint32_t channels) {
    auto reader = mradm::audio::FloatWavReader::open(path.string());
    if (!reader || reader->channels() != channels || reader->frame_count() != k_frames) {
        return std::nullopt;
    }
    std::vector<float> samples(static_cast<std::size_t>(k_frames) * channels);
    reader->read(samples.data(), k_frames);
    return samples;
}

std::optional<std::vector<float>> pull_stream(mradm::IRenderer& renderer,
                                              const mradm::IPreparedRender& prepared,
                                              const mradm::RenderPlan& plan,
                                              const std::vector<std::size_t>& chunks,
                                              const mradm::LiveOverrides* overrides = nullptr) {
    mradm::NullLogSink logs;
    auto stream = renderer.open_stream(prepared, plan, logs);
    if (!stream) {
        std::cerr << "FAIL: matrix stream open failed: " << stream.error().message << '\n';
        return std::nullopt;
    }
    if (overrides != nullptr) {
        (*stream)->set_overrides(*overrides);
    }

    const std::size_t channels = (*stream)->out_channels();
    std::vector<float> output;
    std::size_t chunk_index = 0U;
    while (true) {
        const std::size_t frames = chunks[chunk_index % chunks.size()];
        ++chunk_index;
        std::vector<float> buffer(frames * channels, 0.0F);
        auto produced = (*stream)->process(std::span<float>{buffer}, frames);
        if (!produced) {
            std::cerr << "FAIL: matrix stream process failed: " << produced.error().message << '\n';
            return std::nullopt;
        }
        if (*produced == 0U) {
            break;
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*produced * channels));
        if (output.size() > static_cast<std::size_t>(k_frames) * channels) {
            return std::nullopt;
        }
    }
    return output;
}

double max_difference(std::span<const float> lhs, std::span<const float> rhs) {
    if (lhs.size() != rhs.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double maximum = 0.0;
    for (std::size_t index = 0U; index < lhs.size(); ++index) {
        maximum = std::max(maximum, std::abs(static_cast<double>(lhs[index]) - static_cast<double>(rhs[index])));
    }
    return maximum;
}

bool verify_dynamic_samples(std::string_view backend,
                            const std::vector<float>& rendered,
                            const std::vector<float>& input,
                            uint64_t direct_delay) {
    if (!check(rendered.size() == static_cast<std::size_t>(k_frames) * k_5_1_channels,
               std::string{backend} + ": dynamic output dimensions")) {
        return false;
    }

    constexpr float k_split_left = 0.5F;
    constexpr float k_split_right = 0.8660254037844386F;
    double maximum_error = 0.0;
    bool non_targets_are_zero = true;
    bool mute_is_zero = true;
    for (uint64_t frame = 0U; frame < k_frames; ++frame) {
        std::array<float, k_5_1_channels> expected{};
        const bool has_source_frame = frame >= direct_delay;
        const uint64_t source_frame = has_source_frame ? frame - direct_delay : 0U;
        if (has_source_frame && source_frame < k_first_boundary) {
            expected[0] = input[source_frame] * k_object_gain * 0.5F * k_split_left;
            expected[1] = input[source_frame] * k_object_gain * 0.5F * k_split_right;
        } else if (has_source_frame && source_frame < k_second_boundary) {
            expected[2] = input[source_frame] * k_object_gain * 0.25F;
        }
        for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
            const float actual = rendered[(static_cast<std::size_t>(frame) * expected.size()) + channel];
            maximum_error = std::max(maximum_error,
                                     std::abs(static_cast<double>(actual) - static_cast<double>(expected.at(channel))));
            if (expected.at(channel) == 0.0F && actual != 0.0F) {
                non_targets_are_zero = false;
            }
            if (frame >= k_second_boundary + direct_delay && actual != 0.0F) {
                mute_is_zero = false;
            }
        }
    }

    bool ok = check(maximum_error < 2.0e-6, std::string{backend} + ": matrix coefficients and gains are exact");
    ok &= check(non_targets_are_zero, std::string{backend} + ": non-target channels have no leakage");
    ok &= check(mute_is_zero, std::string{backend} + ": explicit mute is exact silence");
    const uint64_t output_boundary = k_first_boundary + direct_delay;
    ok &= check(rendered[(static_cast<std::size_t>(output_boundary - 1U) * k_5_1_channels)] != 0.0F &&
                    rendered[(static_cast<std::size_t>(output_boundary) * k_5_1_channels) + 2U] != 0.0F,
                std::string{backend} + ": route changes at the exact metadata boundary");
    return ok;
}

bool verify_lfe_samples(std::string_view backend,
                        const std::vector<float>& rendered,
                        const std::vector<float>& input,
                        uint64_t direct_delay) {
    constexpr std::size_t k_channels = 24U;
    constexpr std::size_t k_lfe1 = 3U;
    constexpr std::size_t k_lfe2 = 9U;
    constexpr float k_expected_gain = 0.7F * 0.6F * 0.7071067811865475F;
    if (!check(rendered.size() == static_cast<std::size_t>(k_frames) * k_channels,
               std::string{backend} + ": LFE output dimensions")) {
        return false;
    }

    double maximum_error = 0.0;
    bool other_channels_zero = true;
    for (uint64_t frame = 0U; frame < k_frames; ++frame) {
        const float expected = frame >= direct_delay ? input[frame - direct_delay] * k_expected_gain : 0.0F;
        for (std::size_t channel = 0U; channel < k_channels; ++channel) {
            const float actual = rendered[(static_cast<std::size_t>(frame) * k_channels) + channel];
            const float target = channel == k_lfe1 || channel == k_lfe2 ? expected : 0.0F;
            maximum_error =
                std::max(maximum_error, std::abs(static_cast<double>(actual) - static_cast<double>(target)));
            if (channel != k_lfe1 && channel != k_lfe2 && actual != 0.0F) {
                other_channels_zero = false;
            }
        }
    }
    bool ok =
        check(maximum_error < 2.0e-6, std::string{backend} + ": LFE bypass keeps split-power routing in matrix mode");
    ok &= check(other_channels_zero, std::string{backend} + ": LFE bypass never enters matrix/non-LFE targets");
    return ok;
}

bool verify_backend(std::string_view name,
                    std::unique_ptr<mradm::IRenderer> renderer,
                    const std::filesystem::path& input_path,
                    const std::vector<float>& input_samples,
                    const std::shared_ptr<const mradm::DirectSpeakersMatrix>& dynamic_matrix,
                    const std::shared_ptr<const mradm::DirectSpeakersMatrix>& lfe_matrix,
                    uint64_t direct_delay = 0U) {
    mradm::RenderPlan plan;
    plan.input_path = input_path.string();
    plan.output_layout = "0+5+0";
    plan.scene = make_dynamic_scene();
    plan.direct_speakers_routing_mode = mradm::DirectSpeakersRoutingMode::matrix;
    plan.direct_speakers_matrix = dynamic_matrix;

    const auto output_path = temp_path(std::string{"mradm-matrix-"} + std::string{name});
    FileGuard output_guard{output_path};
    plan.output_path = output_path.string();

    mradm::NullLogSink logs;
    mradm::NullProgressSink progress;
    auto prepared = renderer->prepare(plan, logs);
    if (!prepared) {
        std::cerr << "FAIL: " << name << " matrix prepare failed: " << prepared.error().message << '\n';
        return false;
    }
    const auto rendered = renderer->render_window(**prepared, plan, progress, logs);
    if (!rendered) {
        std::cerr << "FAIL: " << name << " matrix offline render failed: " << rendered.error().message << '\n';
        return false;
    }
    const auto reference = read_output(output_path, k_5_1_channels);
    if (!reference.has_value()) {
        check(false, std::string{name} + ": matrix output opens");
        return false;
    }
    const auto& reference_samples = reference.value();

    bool ok = verify_dynamic_samples(name, reference_samples, input_samples, direct_delay);
    const auto uniform = pull_stream(*renderer, **prepared, plan, {1024U});
    const auto varied = pull_stream(*renderer, **prepared, plan, {333U, 1000U, 7U, 512U});
    if (!uniform.has_value() || !varied.has_value()) {
        return false;
    }
    ok &= check(max_difference(uniform.value(), reference_samples) < 2.0e-6,
                std::string{name} + ": stream matches offline output");
    ok &= check(uniform.value() == varied.value(), std::string{name} + ": stream is independent of pull chunking");

    mradm::LiveObjectOverride object_override;
    object_override.object_id = "AO_matrix_dynamic";
    object_override.gain_db = -6.020599913F;
    mradm::LiveOverrides overrides;
    overrides.revision = 1U;
    overrides.objects.push_back(std::move(object_override));
    const auto gained = pull_stream(*renderer, **prepared, plan, {257U, 1024U}, &overrides);
    if (!gained.has_value() || gained->size() != reference_samples.size()) {
        return false;
    }
    std::vector<float> expected_gained(reference_samples.size());
    std::ranges::transform(reference_samples, expected_gained.begin(), [](float sample) { return sample * 0.5F; });
    ok &= check(max_difference(gained.value(), expected_gained) < 2.0e-6,
                std::string{name} + ": live gain multiplies the prepared matrix result once");

    mradm::RenderPlan lfe_plan;
    lfe_plan.input_path = input_path.string();
    lfe_plan.output_layout = "9+10+3";
    lfe_plan.scene = make_lfe_scene();
    lfe_plan.direct_speakers_routing_mode = mradm::DirectSpeakersRoutingMode::matrix;
    lfe_plan.direct_speakers_matrix = lfe_matrix;
    lfe_plan.lfe_routing_mode = mradm::LfeRoutingMode::split_power;
    const auto lfe_output_path = temp_path(std::string{"mradm-matrix-lfe-"} + std::string{name});
    FileGuard lfe_output_guard{lfe_output_path};
    lfe_plan.output_path = lfe_output_path.string();

    auto lfe_prepared = renderer->prepare(lfe_plan, logs);
    if (!lfe_prepared) {
        std::cerr << "FAIL: " << name << " matrix LFE prepare failed: " << lfe_prepared.error().message << '\n';
        return false;
    }
    const auto lfe_rendered = renderer->render_window(**lfe_prepared, lfe_plan, progress, logs);
    if (!lfe_rendered) {
        std::cerr << "FAIL: " << name << " matrix LFE render failed: " << lfe_rendered.error().message << '\n';
        return false;
    }
    const auto lfe_output = read_output(lfe_output_path, 24U);
    if (!lfe_output.has_value()) {
        check(false, std::string{name} + ": matrix LFE output opens");
        return false;
    }
    ok &= verify_lfe_samples(name, lfe_output.value(), input_samples, direct_delay);
    return ok;
}

} // namespace

int main() {
    const auto input_samples = make_input_samples();
    const auto input_path = temp_path("mradm-matrix-input");
    FileGuard input_guard{input_path};
    if (!write_input(input_path, input_samples)) {
        return EXIT_FAILURE;
    }

    const auto dynamic_matrix = parse_profile(k_dynamic_matrix_json);
    const auto lfe_matrix = parse_profile(k_lfe_matrix_json);
    if (dynamic_matrix == nullptr || lfe_matrix == nullptr) {
        return EXIT_FAILURE;
    }

    bool ok = verify_backend(
        "EAR", mradm::create_ear_renderer(), input_path, input_samples, dynamic_matrix, lfe_matrix, 255U);
    ok &= verify_backend("SAF", mradm::create_vbap_renderer(), input_path, input_samples, dynamic_matrix, lfe_matrix);
#ifdef __APPLE__
    ok &=
        verify_backend("Apple", mradm::create_apple_renderer(), input_path, input_samples, dynamic_matrix, lfe_matrix);
#endif

    if (ok) {
        std::cout << "DirectSpeakers matrix renderer test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
