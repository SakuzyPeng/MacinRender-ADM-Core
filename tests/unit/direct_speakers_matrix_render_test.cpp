// The Apple island's DirectSpeakers matrix rendering (R3 F3: without the C++ SDK). The matrix is
// parsed by mradm-scene through the test FFI and handed to the island settings as the engine does;
// prepare, the offline render and the monitor stream run on mradm_apple_islands(). The Rust
// backends' matrix cases are Rust tests (mradm-render tests/vbap.rs and tests/ear.rs). macOS only.
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

#include "../support/apple_island_driver.h"
#include "../support/direct_speakers_fixture.h"
#include "config_test_ffi.h"

namespace {

constexpr uint32_t k_sample_rate = 48000U;
constexpr uint64_t k_frames = 12317U;
constexpr uint64_t k_first_boundary = 4099U;
constexpr uint64_t k_second_boundary = 8203U;
constexpr uint32_t k_5_1_channels = 6U;
constexpr float k_object_gain = 0.8F;
// write_mono_direct_speakers authors one object, AO_1001 after reassign_ids.
constexpr const char* k_object_id = "AO_1001";

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

// A matrix profile parsed by mradm-scene; the view borrows from the handle.
class Matrix {
  public:
    explicit Matrix(std::string_view json) {
        std::array<char, 1024> message{};
        std::size_t message_len = message.size();
        if (mradm_scene_direct_matrix_parse(reinterpret_cast<const uint8_t*>(json.data()),
                                            json.size(),
                                            &handle_,
                                            &view_,
                                            message.data(),
                                            &message_len) != 0) {
            std::cerr << "FAIL: matrix profile parse failed: " << std::string_view{message.data(), message_len} << '\n';
            handle_ = nullptr;
        }
    }
    ~Matrix() { mradm_scene_direct_matrix_destroy(handle_); }
    Matrix(const Matrix&) = delete;
    Matrix& operator=(const Matrix&) = delete;
    Matrix(Matrix&&) = delete;
    Matrix& operator=(Matrix&&) = delete;

    [[nodiscard]] bool ok() const { return handle_ != nullptr; }
    [[nodiscard]] const MradmDirectMatrixView& view() const { return view_; }

  private:
    MradmDirectMatrix* handle_{nullptr};
    MradmDirectMatrixView view_{};
};

std::optional<std::vector<float>> render_offline(const apple_test::Prepared& prepared,
                                                 const apple_test::Settings& settings,
                                                 std::string_view stem,
                                                 uint32_t channels) {
    const auto output = temp_path(stem);
    FileGuard output_guard{output};
    apple_test::Caller caller;
    const auto reply = apple_test::render(prepared, settings, output, caller);
    if (!reply.ok()) {
        std::cerr << "FAIL: matrix offline render failed: " << reply.message << '\n';
        return std::nullopt;
    }
    auto audio = apple_test::read_wav(output);
    if (!audio || audio->channels != channels || audio->frames != k_frames) {
        return std::nullopt;
    }
    return std::move(audio->samples);
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

bool verify_dynamic_samples(const std::vector<float>& rendered, const std::vector<float>& input) {
    if (!check(rendered.size() == static_cast<std::size_t>(k_frames) * k_5_1_channels,
               "Apple: dynamic output dimensions")) {
        return false;
    }
    constexpr float k_split_left = 0.5F;
    constexpr float k_split_right = 0.8660254037844386F;
    double maximum_error = 0.0;
    bool non_targets_are_zero = true;
    bool mute_is_zero = true;
    for (uint64_t frame = 0U; frame < k_frames; ++frame) {
        std::array<float, k_5_1_channels> expected{};
        if (frame < k_first_boundary) {
            expected[0] = input[frame] * k_object_gain * 0.5F * k_split_left;
            expected[1] = input[frame] * k_object_gain * 0.5F * k_split_right;
        } else if (frame < k_second_boundary) {
            expected[2] = input[frame] * k_object_gain * 0.25F;
        }
        for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
            const float actual = rendered[(static_cast<std::size_t>(frame) * expected.size()) + channel];
            maximum_error = std::max(maximum_error,
                                     std::abs(static_cast<double>(actual) - static_cast<double>(expected.at(channel))));
            if (expected.at(channel) == 0.0F && actual != 0.0F) {
                non_targets_are_zero = false;
            }
            if (frame >= k_second_boundary && actual != 0.0F) {
                mute_is_zero = false;
            }
        }
    }
    bool ok = check(maximum_error < 2.0e-6, "Apple: matrix coefficients and gains are exact");
    ok &= check(non_targets_are_zero, "Apple: non-target channels have no leakage");
    ok &= check(mute_is_zero, "Apple: explicit mute is exact silence");
    ok &= check(rendered[(static_cast<std::size_t>(k_first_boundary - 1U) * k_5_1_channels)] != 0.0F &&
                    rendered[(static_cast<std::size_t>(k_first_boundary) * k_5_1_channels) + 2U] != 0.0F,
                "Apple: route changes at the exact metadata boundary");
    return ok;
}

bool verify_lfe_samples(const std::vector<float>& rendered, const std::vector<float>& input) {
    constexpr std::size_t k_channels = 24U;
    constexpr std::size_t k_lfe1 = 3U;
    constexpr std::size_t k_lfe2 = 9U;
    constexpr float k_expected_gain = 0.7F * 0.6F * 0.7071067811865475F;
    if (!check(rendered.size() == static_cast<std::size_t>(k_frames) * k_channels, "Apple: LFE output dimensions")) {
        return false;
    }
    double maximum_error = 0.0;
    bool other_channels_zero = true;
    for (uint64_t frame = 0U; frame < k_frames; ++frame) {
        const float expected = input[frame] * k_expected_gain;
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
    bool ok = check(maximum_error < 2.0e-6, "Apple: LFE bypass keeps split-power routing in matrix mode");
    ok &= check(other_channels_zero, "Apple: LFE bypass never enters matrix/non-LFE targets");
    return ok;
}

bool verify_apple(const std::filesystem::path& input_path,
                  const std::filesystem::path& lfe_input_path,
                  const std::vector<float>& input_samples,
                  const Matrix& dynamic_matrix,
                  const Matrix& lfe_matrix) {
    apple_test::Settings settings;
    settings.input = input_path.string();
    settings.layout = "0+5+0";
    settings.routing = 3U; // matrix
    settings.matrix = dynamic_matrix.view();
    const apple_test::Prepared prepared(settings);
    if (!prepared.ok()) {
        std::cerr << "FAIL: Apple matrix prepare failed: " << prepared.reply.message << '\n';
        return false;
    }
    const auto reference = render_offline(prepared, settings, "mradm-matrix-Apple", k_5_1_channels);
    if (!reference) {
        return check(false, "Apple: matrix output opens");
    }
    bool ok = verify_dynamic_samples(*reference, input_samples);
    const auto uniform = apple_test::stream_all(prepared, settings, {1024U});
    const auto varied = apple_test::stream_all(prepared, settings, {333U, 1000U, 7U, 512U});
    if (!uniform || !varied) {
        return check(false, "Apple: matrix streams render");
    }
    ok &= check(max_difference(*uniform, *reference) < 2.0e-6, "Apple: stream matches offline output");
    ok &= check(*uniform == *varied, "Apple: stream is independent of pull chunking");

    const auto gained = apple_test::stream_all(
        prepared, settings, {257U, 1024U}, {{k_object_id, -6.020599913F, false, std::nullopt, ""}});
    if (!gained || gained->size() != reference->size()) {
        return check(false, "Apple: gained matrix stream renders");
    }
    std::vector<float> expected_gained(reference->size());
    std::ranges::transform(*reference, expected_gained.begin(), [](float sample) { return sample * 0.5F; });
    ok &= check(max_difference(*gained, expected_gained) < 2.0e-6,
                "Apple: live gain multiplies the prepared matrix result once");

    apple_test::Settings lfe_settings;
    lfe_settings.input = lfe_input_path.string();
    lfe_settings.layout = "9+10+3";
    lfe_settings.routing = 3U; // matrix
    lfe_settings.matrix = lfe_matrix.view();
    lfe_settings.split_power = true;
    const apple_test::Prepared lfe_prepared(lfe_settings);
    if (!lfe_prepared.ok()) {
        std::cerr << "FAIL: Apple matrix LFE prepare failed: " << lfe_prepared.reply.message << '\n';
        return false;
    }
    const auto lfe_output = render_offline(lfe_prepared, lfe_settings, "mradm-matrix-lfe-Apple", 24U);
    if (!lfe_output) {
        return check(false, "Apple: matrix LFE output opens");
    }
    ok &= verify_lfe_samples(*lfe_output, input_samples);
    return ok;
}

} // namespace

int main() {
    const auto input_samples = make_input_samples();
    const auto input_path = temp_path("mradm-matrix-input");
    const auto lfe_input_path = temp_path("mradm-matrix-lfe-input");
    FileGuard input_guard{input_path};
    FileGuard lfe_input_guard{lfe_input_path};
    fixture::write_mono_direct_speakers(
        input_path,
        k_sample_rate,
        input_samples,
        k_object_gain,
        {fixture::direct_speakers_block("M+000", 0U, k_first_boundary, k_sample_rate, 0.5F),
         fixture::direct_speakers_block("U+000", k_first_boundary, k_second_boundary, k_sample_rate, 0.25F),
         fixture::direct_speakers_block("M+110", k_second_boundary, k_frames, k_sample_rate, 0.75F)});
    fixture::write_mono_direct_speakers(lfe_input_path,
                                        k_sample_rate,
                                        input_samples,
                                        0.7F,
                                        {fixture::direct_speakers_block("RC_LFE", 0U, k_frames, k_sample_rate, 0.6F)},
                                        120.0F);
    const Matrix dynamic_matrix(k_dynamic_matrix_json);
    const Matrix lfe_matrix(k_lfe_matrix_json);
    if (!dynamic_matrix.ok() || !lfe_matrix.ok()) {
        return EXIT_FAILURE;
    }
    if (verify_apple(input_path, lfe_input_path, input_samples, dynamic_matrix, lfe_matrix)) {
        std::cout << "DirectSpeakers matrix renderer test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
