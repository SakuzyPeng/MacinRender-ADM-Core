#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>

#include "adm/scene.h"

#include "../reference/scene_numeric/spatial.h"
#include "../reference/scene_numeric/transitions.h"
#include "head_rotation.h"
#include "render_common.h"
#include "scene_math.h"
#include "scene_transition.h"

namespace {
namespace ref = scene_numeric_reference;
using namespace mradm;
class SceneNumericChecks {
    int sample_case = 0;
    size_t sample_point = 0;
    size_t compared = 0;
    double maximum = 0;
    int failures = 0;
    void check(bool good, std::string_view label) {
        if (!good) {
            if (failures < 20) {
                std::cerr << "FAIL " << label << '\n';
            }
            ++failures;
        }
    }
    void compare(float actual, float expected, std::string_view label, bool exact = false) {
        ++compared;
        const double error = std::abs(double(actual) - double(expected));
        maximum = std::max(maximum, error);
        if (!exact && error > 2e-6 + (2e-6 * std::abs(double(expected)))) {
            std::cerr << "case=" << sample_case << " point=" << sample_point << " actual=" << actual
                      << " expected=" << expected << " ";
        }
        check(exact ? std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(expected)
                    : error <= 2e-6 + (2e-6 * std::abs(double(expected))),
              label);
    }
    // NOLINTNEXTLINE(readability-function-size): preserve one ordered geometry comparison matrix.
    void spatial() {
        dsp::SceneRotation apple_rotation{{0.0F, 0.0F, 0.0F}};
        const std::vector<SceneOutputSpeaker> speakers{
            {30, 0, false}, {-30, 0, false}, {0, 0, true}, {110, 0, false}, {-110, 0, false}, {0, 90, false}};
        for (int i = 0; i < 400; ++i) {
            sample_case = i;
            SceneBlockPosition pos;
            pos.cartesian = (i % 2) == 0;
            pos.x = float(((i * 37) % 101) - 50) / 33;
            pos.y = float(((i * 19) % 103) - 51) / 47;
            pos.z = float(((i * 29) % 107) - 53) / 51;
            pos.azimuth = float(((i * 43) % 721) - 360);
            pos.elevation = float(((i * 17) % 181) - 90);
            pos.distance = float(i % 17) / 8;
            if (i == 0) {
                pos.x = pos.y = pos.z = 0;
            }
            const auto p = scene_position_to_polar(pos);
            const auto old = ref::legacy_scene_position_to_polar(pos);
            compare(p.azimuth, old.azimuth, "polar az");
            compare(p.elevation, old.elevation, "polar el");
            compare(p.distance, old.distance, "polar distance", true);
            const auto d = direction_vector_from_position(pos);
            const auto rd = ref::legacy_direction_vector_from_position(pos);
            compare(d.x, rd.x, "direction x");
            compare(d.y, rd.y, "direction y");
            compare(d.z, rd.z, "direction z");
            check(nearest_non_lfe_speaker_index(pos, speakers) ==
                      ref::legacy_nearest_non_lfe_speaker_index(pos, speakers),
                  "nearest route");
            SceneObjectBlock block;
            block.position = pos;
            block.channel_lock = true;
            block.channel_lock_max_distance = float(i % 19) / 10;
            const auto lock = apply_channel_lock(block, speakers);
            const auto rl = ref::legacy_apply_channel_lock(block, speakers);
            check(lock.position.cartesian == rl.position.cartesian, "lock branch");
            compare(lock.position.azimuth, rl.position.azimuth, "lock az");
            block.divergence = float(i % 13) / 12;
            block.divergence_azimuth_range = float(i % 121);
            block.divergence_position_range = float(i % 7) / 6;
            block.width = float(i % 11) / 10;
            block.height = float(i % 7) / 6;
            block.depth = float(i % 5) / 4;
            block.gain = 0.75F;
            const auto branches = expand_object_divergence(block);
            const auto rb = ref::legacy_expand_object_divergence(block);
            check(branches.size() == rb.size(), "divergence branch count");
            for (size_t j = 0; j < branches.size(); ++j) {
                compare(branches[j].position.azimuth, rb[j].position.azimuth, "divergence az");
                compare(branches[j].gain, rb[j].gain, "divergence gain", true);
            }
            const auto cloud = render_common::extent_disk_cloud(pos, block.width, block.height, block.depth);
            const auto rc = ref::extent_disk_cloud(pos, block.width, block.height, block.depth);
            check(cloud.size() == rc.size(), "cloud count");
            for (size_t j = 0; j < cloud.size(); ++j) {
                sample_point = j;
                compare(cloud.at(j).azimuth, rc[j].azimuth, "cloud az");
                compare(cloud.at(j).elevation, rc[j].elevation, "cloud el");
                compare(cloud.at(j).weight, rc[j].weight, "cloud weight", true);
            }
            std::array<MradmSceneCloudPoint, 17> binaural{};
            size_t count = 0;
            const std::array input{pos.cartesian ? pos.x : pos.azimuth,
                                   pos.cartesian ? pos.y : pos.elevation,
                                   pos.cartesian ? pos.z : pos.distance,
                                   block.width,
                                   block.height,
                                   block.depth,
                                   block.gain};
            check(mradm_dsp_scene_cloud(input.data(),
                                        input.size(),
                                        pos.cartesian ? 1U : 0U,
                                        1U | dsp::scene_cpp_contract,
                                        binaural.data(),
                                        binaural.size(),
                                        &count,
                                        nullptr,
                                        0) == 0,
                  "binaural cloud call");
            const auto bc = ref::expand_binaural_extent(block, block.gain, BinauralSpreadMode::automatic);
            check(count == bc.size(), "binaural cloud count");
            for (size_t j = 0; j < count; ++j) {
                sample_point = j;
                compare(binaural.at(j).azimuth, bc[j].azimuth, "binaural cloud az");
                compare(binaural.at(j).elevation, bc[j].elevation, "binaural cloud el");
                compare(binaural.at(j).weight, bc[j].gain, "binaural gain", true);
                check(binaural.at(j).slot == bc[j].slot, "binaural slot");
            }
            const ListenerOrientation orientation{float((i % 121) - 60), float((i % 91) - 45), float((i % 61) - 30)};
            render_common::HeadRotation rotation{orientation};
            ref::HeadRotation old_rotation{orientation};
            const auto r = rotation.rotate_az_el(p.azimuth, p.elevation);
            const auto rr = old_rotation.rotate_az_el(p.azimuth, p.elevation);
            compare(r.first, rr.first, "rotation az");
            compare(r.second, rr.second, "rotation el");
            const std::array pose{
                p.azimuth, p.elevation, orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg};
            std::array<float, 2> apple{};
            check(mradm_dsp_scene_rotate_pose(
                      pose.data(), pose.size(), apple.data(), apple.size(), 1U | dsp::scene_cpp_contract) == 0,
                  "apple rotation call");
            const auto ar = ref::head_lock_compensate(p.azimuth, p.elevation, orientation);
            compare(apple[0], ar.first, "apple az");
            compare(apple[1], ar.second, "apple el");
            apple_rotation.update({orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg});
            const auto prepared = apple_rotation.apply(p.azimuth, p.elevation, true);
            compare(prepared.first, ar.first, "prepared apple az");
            compare(prepared.second, ar.second, "prepared apple el");
        }
    }
    void transitions() {
        for (const size_t channels : {1U, 2U, 65U}) {
            for (const uint32_t rate : {1U, 8000U, 44100U, 48000U, 192000U}) {
                dsp::SceneTransitions actual(channels, rate, 2048);
                ref::Transitions expected(channels, rate);
                std::vector<float> left(channels * 2051);
                std::vector<float> right(left.size());
                std::vector<float> reference;
                std::vector<float> last(channels);
                std::vector<float> anchor(channels);
                for (size_t i = 0; i < left.size(); ++i) {
                    right[i] = float(i % 53) / 61;
                }
                actual.begin_generation();
                expected.begin_generation();
                for (const uint32_t frames : {0U, 1U, 31U, 512U, 2047U, 7U, 2051U}) {
                    for (size_t i = 0; i < left.size(); ++i) {
                        left[i] = float(i % 37) / 71;
                    }
                    reference = left;
                    check(actual.mix(left, right, frames) == expected.mix(reference, right, frames),
                          "transition completion");
                    for (size_t i = 0; i < frames * channels; ++i) {
                        compare(left[i], reference[i], "backend PCM");
                    }
                    const bool silence = frames == 31U;
                    actual.process_output(left, frames, silence);
                    expected.apply_transition(reference.data(), frames, silence);
                    for (size_t i = 0; i < frames * channels; ++i) {
                        compare(left[i], reference[i], "generation PCM");
                    }
                    MradmSceneTransitionStatus snapshot{};
                    check(mradm_dsp_scene_transition_snapshot(
                              actual.get(), last.data(), last.size(), anchor.data(), anchor.size(), &snapshot) == 0,
                          "transition snapshot");
                    for (size_t channel = 0; channel < channels; ++channel) {
                        compare(last[channel], expected.last_output_frame[channel], "last output history");
                        compare(anchor[channel], expected.transition_anchor[channel], "generation anchor");
                    }
                    const auto status = actual.status();
                    check(status.backend_position == expected.backend_crossfade_position &&
                              status.generation_position == expected.transition_position &&
                              status.generation_remaining == expected.transition_remaining,
                          "transition counters");
                    if (frames == 512U) {
                        actual.begin_generation();
                        expected.begin_generation();
                    }
                }
                auto moved = std::move(actual);
                moved.reset();
                check(moved.status().backend_position == 0, "transition move/reset");
            }
        }
    }
    static float runtime_float(float value) {
        const volatile float loaded = value;
        return loaded;
    }
    void pose_boundaries() {
        for (float yaw : {-540.0F, -180.0F, -0.0F, 0.0F, 90.0F, 180.0F, 540.0F, 1.0e30F}) {
            for (float pitch : {-90.0F, -89.99992F, -45.0F, 0.0F, 45.0F, 89.99992F, 90.0F}) {
                const std::array input{yaw, pitch, 30.0F};
                std::array<float, 7> output{};
                const auto old = ref::legacy_pose(input, false);
                check(old.has_value() && mradm_dsp_scene_pose(input.data(), 3, output.data(), 7, 0) == 0,
                      "pose boundary conversion");
                if (!old) {
                    continue;
                }
                for (size_t i = 0; i < 4; ++i) {
                    compare(output.at(i), old->quaternion_xyzw.at(i), "pose quaternion");
                }
                for (size_t i = 0; i < 3; ++i) {
                    compare(output.at(i + 4), old->euler_deg.at(i), "pose euler");
                }
                std::array<float, 4> quaternion{output[0], output[1], output[2], output[3]};
                for (float scale : {-2.0F, 0.0001F, 1.0F, 10000.0F}) {
                    auto q = quaternion;
                    std::ranges::transform(q, q.begin(), [scale](float value) { return value * scale; });
                    const auto expected = ref::legacy_pose(q, true);
                    const int status = mradm_dsp_scene_pose(q.data(), 4, output.data(), 7, 1);
                    check(expected.has_value() == (status == 0), "quaternion acceptance");
                    if (expected) {
                        for (size_t i = 0; i < 4; ++i) {
                            compare(output.at(i), expected->quaternion_xyzw.at(i), "normalized quaternion");
                        }
                        for (size_t i = 0; i < 3; ++i) {
                            compare(output.at(i + 4), expected->euler_deg.at(i), "quaternion euler");
                        }
                    }
                }
            }
        }
        for (float az : {-180.0F, -90.0F, -0.0F, 0.0F, 90.0F, 180.0F}) {
            for (float el : {-90.0F, 0.0F, 90.0F}) {
                for (const ListenerOrientation orientation : {ListenerOrientation{},
                                                              ListenerOrientation{90, 0, 0},
                                                              ListenerOrientation{0, 90, 0},
                                                              ListenerOrientation{0, 0, 90}}) {
                    const ListenerOrientation runtime{runtime_float(orientation.yaw_deg),
                                                      runtime_float(orientation.pitch_deg),
                                                      runtime_float(orientation.roll_deg)};
                    render_common::HeadRotation actual{runtime};
                    ref::HeadRotation old{runtime};
                    const float runtime_az = runtime_float(az);
                    const float runtime_el = runtime_float(el);
                    const auto a = actual.rotate_az_el(runtime_az, runtime_el);
                    const auto b = old.rotate_az_el(runtime_az, runtime_el);
                    compare(a.first, b.first, "head pole az");
                    compare(a.second, b.second, "head pole el");
                }
            }
        }
    }
    void lock_boundaries() {
        const std::vector<SceneOutputSpeaker> speakers{{30, 0, false}, {-30, 0, false}, {0, 90, false}};
        for (float az : {-175.0F, -60.0F, -15.0F, 0.0F, 15.0F, 60.0F, 175.0F}) {
            for (float el : {-75.0F, 0.0F, 75.0F}) {
                SceneObjectBlock block;
                block.position.azimuth = runtime_float(az);
                block.position.elevation = runtime_float(el);
                block.channel_lock = true;
                const auto nearest = ref::legacy_nearest_non_lfe_speaker_index(block.position, speakers);
                if (!nearest) {
                    check(false, "nearest speaker exists");
                    continue;
                }
                const auto source = ref::legacy_direction_vector_from_position(block.position);
                const auto speaker = ref::legacy_direction_vector_from_polar(speakers.at(*nearest).azimuth,
                                                                             speakers.at(*nearest).elevation);
                const float boundary = ref::legacy_direction_distance(source, speaker) - 1.0e-4F;
                for (float value : {std::nextafter(boundary, -std::numeric_limits<float>::infinity()),
                                    boundary,
                                    std::nextafter(boundary, std::numeric_limits<float>::infinity())}) {
                    block.channel_lock_max_distance = value;
                    const auto actual = apply_channel_lock(block, speakers);
                    const auto expected = ref::legacy_apply_channel_lock(block, speakers);
                    check(actual.position.azimuth == expected.position.azimuth &&
                              actual.position.elevation == expected.position.elevation,
                          "channel-lock threshold branch");
                }
            }
        }
    }
    void independent() {
        compare(render_common::canonical_vector_length(3, 4, 0), 5, "3-4-5", true);
        check(wrap_azimuth(-180) == 180 && wrap_azimuth(540) == 180, "azimuth seam");
        const std::array pose{0.0F, 0.0F, 0.0F};
        std::array<float, 7> result{};
        check(mradm_dsp_scene_pose(pose.data(), pose.size(), result.data(), result.size(), 0) == 0, "identity pose");
        compare(result[3], 1, "identity quaternion", true);
        std::array<float, 2> output{31, 37};
        const auto before = output;
        check(mradm_dsp_scene_math(2, pose.data(), 3, output.data(), 2) != 0 && output == before,
              "short output atomicity");
        std::array<MradmSceneCloudPoint, 17> points{};
        size_t count = 999;
        check(mradm_dsp_scene_cloud(pose.data(), 3, 0, 0, points.data(), points.size(), &count, nullptr, 0) != 0 &&
                  count == 999,
              "cloud count atomicity");
    }

  public:
    int run(int argc, char** argv) {
        spatial();
        independent();
        pose_boundaries();
        lock_boundaries();
        const auto spatial_compared = compared;
        const auto spatial_maximum = maximum;
        maximum = 0;
        transitions();
        const auto transition_compared = compared - spatial_compared;
        const auto transition_maximum = maximum;
        maximum = std::max(spatial_maximum, transition_maximum);
        std::cout << "Scene spatial comparison: " << compared << " floats, max absolute error " << maximum
                  << ", failures " << failures << '\n';
        if (argc > 1) {
            std::ofstream json{argv[1]};
            json << std::setprecision(17) << R"({"spatial":{"compared":)" << spatial_compared
                 << R"(,"max_absolute_error":)" << spatial_maximum << R"(},"transitions":{"compared":)"
                 << transition_compared << R"(,"max_absolute_error":)" << transition_maximum << R"(},"failures":)"
                 << failures << "}\n";
        }
        return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
};
} // namespace
int main(int argc, char** argv) {
    return SceneNumericChecks{}.run(argc, argv);
}
