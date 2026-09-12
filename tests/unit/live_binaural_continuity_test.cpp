#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "binaural_internal.h"
#include "live_binaural_convolver.h"
#include "live_binaural_renderer.h"

namespace {
using namespace mradm::live_scene;
constexpr double k_omega = 2.0 * std::numbers::pi * 100.0 / 48000.0;

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

std::vector<float_complex> response(std::span<const float> impulse) {
    std::vector<float_complex> result((impulse.size() + 2U), float_complex{0.0F, 0.0F});
    for (std::size_t band = 0U; band <= impulse.size() / 2U; ++band) {
        std::complex<double> value{0.0, 0.0};
        for (std::size_t tap = 0U; tap < impulse.size(); ++tap) {
            const double phase =
                -2.0 * std::numbers::pi * static_cast<double>(band * tap) / static_cast<double>(impulse.size());
            value += static_cast<double>(impulse[tap]) * std::polar(1.0, phase);
        }
        result[band * 2U] = float_complex{static_cast<float>(value.real()), static_cast<float>(value.imag())};
        result[(band * 2U) + 1U] = result[band * 2U];
    }
    return result;
}

bool test_full_fir_and_partitioning() {
    std::array<float, 64U> impulse{};
    impulse[3U] = 0.5F;
    impulse[31U] = -0.2F;
    impulse[63U] = 0.3F;
    const auto hrtf = response(impulse);
    std::array<float, 512U> input{};
    for (std::size_t index = 0U; index < 350U; ++index) {
        input.at(index) = (static_cast<float>((index * 37U) % 101U) / 101.0F) - 0.5F;
    }
    bool ok = true;
    for (const std::size_t block : {1U, 7U, 37U, 64U}) {
        LiveBinauralConvolver convolver(64, 64U, 48000U);
        auto state = convolver.make_state();
        std::vector<float> output(input.size());
        std::vector<float> right(input.size());
        for (std::size_t offset = 0U; offset < input.size();) {
            const auto count = std::min(block, input.size() - offset);
            convolver.process(state,
                              hrtf,
                              std::span{input}.subspan(offset, count),
                              std::span{output}.subspan(offset, count),
                              std::span{right}.subspan(offset, count),
                              false);
            offset += count;
        }
        double error = 0.0;
        for (std::size_t index = 0U; index < input.size(); ++index) {
            double expected = 0.0;
            for (std::size_t tap = 0U; tap < impulse.size() && tap <= index; ++tap) {
                expected += static_cast<double>(impulse.at(tap)) * static_cast<double>(input.at(index - tap));
            }
            error = std::max({error,
                              std::abs(static_cast<double>(output[index]) - expected),
                              std::abs(static_cast<double>(right[index]) - expected)});
        }
        ok &= check(error < 2.0e-7, "all FIR taps match direct convolution for arbitrary partitions and silent tails");
    }
    return ok;
}

bool test_retarget_uses_input_history() {
    std::array<float, 64U> impulse{};
    impulse[63U] = 1.0F;
    const auto initial = response(impulse);
    impulse[63U] = 0.0F;
    impulse[61U] = 0.25F;
    const auto target = response(impulse);
    impulse[61U] = 0.0F;
    impulse[0U] = -0.5F;
    const auto interrupted = response(impulse);
    LiveBinauralConvolver convolver(64, 64U, 1000U); // exactly ten samples per control fade
    auto state = convolver.make_state();
    std::array<float, 64U> ones{};
    ones.fill(1.0F);
    std::array<float, 64U> left{};
    std::array<float, 64U> right{};
    convolver.process(state, initial, ones, left, right, false);
    convolver.process(
        state, target, std::span{ones}.first(3U), std::span{left}.first(3U), std::span{right}.first(3U), false);
    bool ok = true;
    for (std::size_t index = 0U; index < 3U; ++index) {
        ok &= check(std::abs(left.at(index) - (1.0F - (0.075F * static_cast<float>(index)))) < 2.0e-6F,
                    "the new filter includes input preceding its control update");
    }
    convolver.process(
        state, interrupted, std::span{ones}.first(17U), std::span{left}.first(17U), std::span{right}.first(17U), false);
    for (std::size_t index = 0U; index < 17U; ++index) {
        const float alpha = std::min(1.0F, static_cast<float>(index) / 10.0F);
        const float expected = 0.775F + (alpha * (-0.5F - 0.775F));
        ok &= check(std::abs(left.at(index) - expected) < 2.0e-6F,
                    "a fade retargets from its audible state and completes inside a longer block");
    }
    return ok;
}

enum class Motion : std::uint8_t { fixed, head, object, gain };

std::vector<float> render_motion(const std::filesystem::path& sofa,
                                 Motion motion,
                                 std::uint32_t block = 256U,
                                 bool locked = false,
                                 float fixed_yaw = 0.0F) {
    RendererConfig config;
    config.output_layout = "binaural";
    config.sofa_path = sofa;
    auto created = create_live_binaural_renderer(config, {});
    require(created);
    auto renderer = std::move(*created);
    ElementDescriptor descriptor;
    descriptor.element_id = 1U;
    descriptor.has_position = true;
    require(renderer->configure_generation(1U, std::span{&descriptor, 1U}));
    constexpr std::uint32_t k_total = 192000U;
    constexpr std::uint32_t k_start = 48384U;
    constexpr std::uint32_t k_end = 144384U;
    std::vector<float> output(static_cast<std::size_t>(k_total) * 2U);
    for (std::uint32_t cursor = 0U; cursor < k_total;) {
        const auto until_control = 768U - (cursor % 768U);
        const auto frames = std::min({block, k_total - cursor, until_control});
        const auto progress_at = [](std::uint32_t sample) {
            return std::clamp((static_cast<float>(sample) - static_cast<float>(k_start)) /
                                  static_cast<float>(k_end - k_start),
                              0.0F,
                              1.0F);
        };
        if (motion == Motion::head) {
            renderer->set_listener_orientation({90.0F * progress_at(cursor - (cursor % 768U)), 0.0F, 0.0F});
        } else {
            renderer->set_listener_orientation({fixed_yaw, 0.0F, 0.0F});
        }
        Frame frame;
        frame.epoch_id = 1U;
        frame.generation_id = 1U;
        frame.media_sample_start = cursor;
        frame.duration_samples = frames;
        frame.flags = frame_state_complete;
        if (cursor == 0U) {
            ObjectState state;
            state.valid_fields = k_known_state_fields;
            state.head_locked = locked;
            frame.initial_states.push_back({1U, state});
        }
        if ((motion == Motion::object || motion == Motion::gain) && cursor >= k_start && cursor <= k_end &&
            cursor % 768U == 0U) {
            MetadataUpdate update;
            update.element_id = 1U;
            update.changed_fields = motion == Motion::gain ? state_linear_gain : state_position;
            update.state.valid_fields = update.changed_fields;
            update.ramp_duration_samples = 768U;
            const float progress = progress_at(cursor + 768U);
            const float angle = progress * std::numbers::pi_v<float> * 0.5F;
            update.state.x = std::sin(angle);
            update.state.y = std::cos(angle);
            update.state.linear_gain = 1.0F - (0.5F * progress);
            frame.updates.push_back(update);
        }
        PcmPlane plane;
        plane.element_id = 1U;
        plane.has_signal = true;
        plane.samples.resize(frames);
        for (std::uint32_t index = 0U; index < frames; ++index) {
            plane.samples[index] = 0.1F * static_cast<float>(std::sin(k_omega * static_cast<double>(cursor + index)));
        }
        frame.pcm.push_back(std::move(plane));
        require(renderer->render(
            frame,
            std::span{output}.subspan(static_cast<std::size_t>(cursor) * 2U, static_cast<std::size_t>(frames) * 2U)));
        cursor += frames;
    }
    return output;
}

// A stationary 100 Hz tone obeys this recurrence. Motion adds slow amplitude /
// phase modulation, but must not introduce the large broadband impulses that
// the former output-tail crossfade produced (roughly 1e-2 in these fixtures).
double peak_tone_residual(std::span<const float> audio) {
    const double coefficient = 2.0 * std::cos(k_omega);
    double peak = 0.0;
    for (std::size_t index = 96000U; index < 300000U; ++index) {
        const double residual = static_cast<double>(audio[index]) -
                                (coefficient * static_cast<double>(audio[index - 2U])) +
                                static_cast<double>(audio[index - 4U]);
        if (!std::isfinite(residual)) {
            return 1.0;
        }
        peak = std::max(peak, std::abs(residual));
    }
    return peak;
}

bool test_real_hrtf_motion(const std::filesystem::path& sofa) {
    bool ok = true;
    const auto fixed = render_motion(sofa, Motion::fixed);
    const auto locked = render_motion(sofa, Motion::head, 256U, true);
    ok &= check(fixed == locked, "head-locked audio ignores live head movement");
    for (const auto motion : {Motion::head, Motion::object, Motion::gain}) {
        const auto audio = render_motion(sofa, motion);
        const auto residual = peak_tone_residual(audio);
        float change = 0.0F;
        float peak = 0.0F;
        for (std::size_t index = 96000U; index < audio.size(); ++index) {
            change = std::max(change, std::abs(audio[index] - fixed[index]));
            peak = std::max(peak, std::abs(audio[index]));
        }
        ok &= check(change > 1.0e-3F && peak > 1.0e-3F, "motion changes audible PCM instead of freezing or muting it");
        std::cout << (sofa.empty() ? "KEMAR" : "SOFA") << " motion=" << static_cast<int>(motion)
                  << " peak residual=" << residual << '\n';
        ok &= check(residual < 5.0e-5, "head / object / gain motion has no large sample discontinuities");
    }
    const auto regular = render_motion(sofa, Motion::fixed, 512U, false, 37.0F);
    const auto irregular = render_motion(sofa, Motion::fixed, 137U, false, 37.0F);
    float error = 0.0F;
    for (std::size_t index = 0U; index < regular.size(); ++index) {
        error = std::max(error, std::abs(regular[index] - irregular[index]));
    }
    ok &= check(error < 2.0e-7F, "off-measurement-grid HRTF convolution is independent of frame partitioning");
    return ok;
}

bool test_continuous_lookup() {
    using namespace mradm::binaural_internal;
    auto state = build_binaural_state(built_in_kemar_dataset(), 1024U);
    if (!check(state != nullptr, "prepare interpolation state")) {
        return false;
    }
    std::vector<float_complex> left;
    std::vector<float_complex> middle;
    std::vector<float_complex> right;
    compute_continuous_hrtf_into(*state, 0.49F, 0.0F, left);
    compute_continuous_hrtf_into(*state, 0.50F, 0.0F, middle);
    compute_continuous_hrtf_into(*state, 0.51F, 0.0F, right);
    float error = 0.0F;
    float movement = 0.0F;
    for (std::size_t index = 0U; index < middle.size(); ++index) {
        error = std::max(error, std::abs(middle[index] - (left.at(index) + right[index]) * 0.5F));
        movement = std::max(movement, std::abs(left.at(index) - right[index]));
    }
    bool ok =
        check(error < 2.0e-6F && movement > 1.0e-5F, "sub-degree directions interpolate through former rounding edges");
    compute_continuous_hrtf_into(*state, -180.0F, 20.0F, left);
    compute_continuous_hrtf_into(*state, 180.0F, 20.0F, right);
    ok &= check(left == right, "azimuth seam is periodic");
    compute_continuous_hrtf_into(*state, 37.0F, 0.0F, left);
    compute_hrtf_into(*state, 37.0F, 0.0F, right);
    ok &= check(left == right, "integer directions retain magnitude-preserving HRTF interpolation");
    return ok;
}
} // namespace

int main() {
    try {
        bool ok = test_full_fir_and_partitioning();
        ok &= test_retarget_uses_input_history();
        ok &= test_continuous_lookup();
        ok &= test_real_hrtf_motion({});
        if (const char* sofa = std::getenv("MR_ADM_TEST_SOFA")) {
            ok &= test_real_hrtf_motion(sofa);
        }
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
