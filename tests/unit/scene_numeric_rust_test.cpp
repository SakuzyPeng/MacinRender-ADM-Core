#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>

#include "adm/scene.h"

#include "../reference/scene_numeric/spatial.h"
#include "head_rotation.h"
#include "render_common.h"
#include "scene_math.h"

namespace {
namespace ref = scene_numeric_reference;
using namespace mradm;
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
    if (!exact && error > 2e-6 + 2e-6 * std::abs(double(expected))) {
        std::cerr << "case=" << sample_case << " point=" << sample_point << " actual=" << actual
                  << " expected=" << expected << " ";
    }
    check(exact ? std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(expected)
                : error <= 2e-6 + 2e-6 * std::abs(double(expected)),
          label);
}
void spatial() {
    const std::vector<SceneOutputSpeaker> speakers{
        {30, 0, false}, {-30, 0, false}, {0, 0, true}, {110, 0, false}, {-110, 0, false}, {0, 90, false}};
    for (int i = 0; i < 400; ++i) {
        sample_case = i;
        SceneBlockPosition pos;
        pos.cartesian = (i % 2) == 0;
        pos.x = float((i * 37) % 101 - 50) / 33;
        pos.y = float((i * 19) % 103 - 51) / 47;
        pos.z = float((i * 29) % 107 - 53) / 51;
        pos.azimuth = float((i * 43) % 721 - 360);
        pos.elevation = float((i * 17) % 181 - 90);
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
        check(nearest_non_lfe_speaker_index(pos, speakers) == ref::legacy_nearest_non_lfe_speaker_index(pos, speakers),
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
            compare(cloud[j].azimuth, rc[j].azimuth, "cloud az");
            compare(cloud[j].elevation, rc[j].elevation, "cloud el");
            compare(cloud[j].weight, rc[j].weight, "cloud weight", true);
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
            compare(binaural[j].azimuth, bc[j].azimuth, "binaural cloud az");
            compare(binaural[j].elevation, bc[j].elevation, "binaural cloud el");
            compare(binaural[j].weight, bc[j].gain, "binaural gain", true);
            check(binaural[j].slot == bc[j].slot, "binaural slot");
        }
        const ListenerOrientation orientation{float(i % 121 - 60), float(i % 91 - 45), float(i % 61 - 30)};
        render_common::HeadRotation rotation{orientation};
        ref::HeadRotation old_rotation{orientation};
        const auto r = rotation.rotate_az_el(p.azimuth, p.elevation);
        const auto rr = old_rotation.rotate_az_el(p.azimuth, p.elevation);
        compare(r.first, rr.first, "rotation az");
        compare(r.second, rr.second, "rotation el");
        const std::array pose{p.azimuth, p.elevation, orientation.yaw_deg, orientation.pitch_deg, orientation.roll_deg};
        std::array<float, 2> apple{};
        check(mradm_dsp_scene_rotate_pose(pose.data(), pose.size(), apple.data(), apple.size(), 1) == 0,
              "apple rotation call");
        const auto ar = ref::head_lock_compensate(p.azimuth, p.elevation, orientation);
        compare(apple[0], ar.first, "apple az");
        compare(apple[1], ar.second, "apple el");
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
    check(mradm_dsp_scene_math(2, pose.data(), 3, output.data(), 2) != 0 && output == before, "short output atomicity");
    std::array<MradmSceneCloudPoint, 17> points{};
    size_t count = 999;
    check(mradm_dsp_scene_cloud(pose.data(), 3, 0, 0, points.data(), points.size(), &count, nullptr, 0) != 0 &&
              count == 999,
          "cloud count atomicity");
}
} // namespace
int main(int argc, char** argv) {
    spatial();
    independent();
    std::cout << "Scene spatial comparison: " << compared << " floats, max absolute error " << maximum << ", failures "
              << failures << '\n';
    if (argc > 1) {
        std::ofstream json{argv[1]};
        json << "{\"compared\":" << compared << ",\"max_absolute_error\":" << maximum << ",\"failures\":" << failures
             << "}\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
