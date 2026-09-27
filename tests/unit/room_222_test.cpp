#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <vector>

#include "room_222.h"
#include "room_compat_bed.h"
#include "room_compat_panner.h"
#include "room_compat_size_processor.h"

namespace {
bool check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}

mradm::SceneBlockPosition position(float x, float y, float z) {
    mradm::SceneBlockPosition result;
    result.cartesian = true;
    result.x = x;
    result.y = y;
    result.z = z;
    return result;
}

float random_unit(uint32_t& state) {
    state = (state * 1664525U) + 1013904223U;
    return static_cast<float>(state >> 8U) / 16777216.0F;
}

bool geometry_checks() {
    using namespace mradm::room_compat;
    bool ok = true;
    std::set<std::size_t> channels;
    for (const auto& node : room_222_nodes()) {
        channels.insert(node.channel);
        const auto gains = room_222_gains(position(node.x, node.y, node.z));
        ok &= check(gains.has_value(), "every speaker node is renderable");
        if (!gains) {
            return false;
        }
        for (std::size_t i = 0; i < gains->size(); ++i) {
            ok &= check(std::fabs(gains->at(i) - (i == node.channel ? 1.0F : 0.0F)) < 1e-6F,
                        "real vertices are one-hot, including zenith and all three lower speakers");
        }
    }
    ok &= check(channels.size() == 22 && !channels.contains(3) && !channels.contains(9), "exactly 22 non-LFE nodes");
    constexpr std::array<std::size_t, 24> mirror{1,  0,  2,  3,  5,  4,  7,  6,  8,  9,  11, 10,
                                                 13, 12, 14, 15, 17, 16, 19, 18, 20, 21, 23, 22};
    double max_power_error = 0;
    for (int x = -4; x <= 4; ++x) {
        for (int y = -4; y <= 4; ++y) {
            for (int z = -4; z <= 4; ++z) {
                for (const float size : {0.0F, .00001F, .01F, .19999F, .2F, .75F, 1.0F}) {
                    const auto p =
                        position(static_cast<float>(x) / 4, static_cast<float>(y) / 4, static_cast<float>(z) / 4);
                    const auto gains = room_222_gains(p, size);
                    const auto reversed = room_222_gains(position(-p.x, p.y, p.z), size);
                    if (!gains || !reversed) {
                        return false;
                    }
                    double power = 0;
                    for (std::size_t i = 0; i < gains->size(); ++i) {
                        power += static_cast<double>(gains->at(i)) * gains->at(i);
                        ok &= check(std::isfinite(gains->at(i)) && gains->at(i) >= 0, "finite nonnegative gains");
                        ok &= check(std::fabs(gains->at(i) - reversed->at(mirror.at(i))) < 2e-6F,
                                    "spatial left/right symmetry");
                    }
                    max_power_error = std::max(max_power_error, std::fabs(power - 1));
                    ok &= check(gains->at(3) == 0 && gains->at(9) == 0, "Objects never feed either LFE");
                }
            }
        }
    }
    ok &= check(max_power_error < 3e-6, "point and integrated spatial gains conserve raw power");
    const auto near = room_222_gains(position(0, .1F, 0));
    const auto far = room_222_gains(position(0, 1, 0));
    ok &= check(near && far && *near != *far, "full XYZ retains same-direction radius dependence");
    for (const auto& node : room_222_nodes()) {
        for (const float size : {0.0F, .01F, .2F, 1.0F}) {
            const auto a = room_222_gains(position(node.x, node.y, std::clamp(node.z - 1e-5F, -1.0F, 1.0F)), size);
            const auto b = room_222_gains(position(node.x, node.y, std::clamp(node.z + 1e-5F, -1.0F, 1.0F)), size);
            for (std::size_t i = 0; i < a->size(); ++i) {
                ok &= check(std::fabs(a->at(i) - b->at(i)) < .0002F, "continuous layer/vertex neighborhoods");
            }
        }
    }
    ok &= check(!room_222_gains(position(0, 0, std::numeric_limits<float>::quiet_NaN())), "reject nonfinite geometry");
    ok &= check(!room_222_gains(position(0, 0, -1.01F)), "reject out-of-room lower coordinate");
    std::cout << "raw_power_max_error=" << max_power_error << '\n';
    return ok;
}

bool integral_checks() {
    using namespace mradm::room_compat;
    uint32_t random = 0x2220927U;
    double worst = 0;
    constexpr int k_steps = 24;
    for (int test = 0; test < 32; ++test) {
        const float center_x = (random_unit(random) * 2) - 1;
        const float center_y = (random_unit(random) * 2) - 1;
        const float center_z = (random_unit(random) * 2) - 1;
        const auto center = position(center_x, center_y, center_z);
        const float size = .01F + (random_unit(random) * .99F);
        const auto exact = room_222_gains(center, size);
        const std::array<float, 3> p{center.x, center.y, center.z};
        std::array<float, 3> lo{};
        std::array<float, 3> step{};
        for (std::size_t i = 0; i < 3; ++i) {
            lo.at(i) = std::max(-1.0F, p.at(i) - size);
            step.at(i) = (std::min(1.0F, p.at(i) + size) - lo.at(i)) / k_steps;
        }
        std::array<double, 24> measured{};
        for (int z = 0; z < k_steps; ++z) {
            for (int y = 0; y < k_steps; ++y) {
                for (int x = 0; x < k_steps; ++x) {
                    const auto value =
                        room_222_gains(position(lo.at(0) + ((static_cast<float>(x) + .5F) * step.at(0)),
                                                lo.at(1) + ((static_cast<float>(y) + .5F) * step.at(1)),
                                                lo.at(2) + ((static_cast<float>(z) + .5F) * step.at(2))));
                    for (std::size_t c = 0; c < measured.size(); ++c) {
                        measured.at(c) += static_cast<double>(value->at(c)) * value->at(c);
                    }
                }
            }
        }
        for (std::size_t c = 0; c < measured.size(); ++c) {
            worst = std::max(worst,
                             std::fabs((measured.at(c) / (k_steps * k_steps * k_steps)) -
                                       (static_cast<double>(exact->at(c)) * exact->at(c))));
        }
    }
    std::cout << "independent_midpoint_integral_max_power_error=" << worst << '\n';
    return check(worst < .0015,
                 "analytic size integration matches independent 3D midpoint evaluation at 32 new points");
}

std::vector<float>
run(mradm::room_compat::SizeObjectProcessor& processor, std::span<const float> input, std::size_t chunk) {
    std::vector<float> output;
    for (std::size_t at = 0; at < input.size(); at += chunk) {
        if (!processor.push(input.subspan(at, std::min(chunk, input.size() - at)), output)) {
            return {};
        }
    }
    return processor.finish(output) ? output : std::vector<float>{};
}

bool state_checks() {
    using namespace mradm::room_compat;
    const std::vector<SizeEvent> events{{0, {.5F, .5F, 0}, 0},
                                        {513, {.2F, .8F, -.7F}, .01F},
                                        {2048, {.5F, .5F, 1}, .2F},
                                        {4095, {.8F, .2F, .2F}, 1},
                                        {6144, {.5F, .5F, -1}, 0},
                                        {8193, {.25F, .25F, 0}, .25F}};
    std::vector<float> input(14473);
    uint32_t seed = 0x222A;
    for (std::size_t i = 0; i < 10240; ++i) {
        input[i] = (random_unit(seed) - .5F) * .25F;
    }
    auto first = SizeObjectProcessor::create(events, "22.2", 48000);
    auto second = SizeObjectProcessor::create(events, "22.2", 48000);
    if (!first || !second) {
        return false;
    }
    const auto expected = run(*first, input, 512);
    bool ok = check(expected.size() == input.size() * 24, "size output retains exact input duration");
    for (std::size_t i = 0; i < input.size(); ++i) {
        ok &= check(expected[(i * 24) + 3] == 0 && expected[(i * 24) + 9] == 0, "size has no LFE leakage");
        if (i > input.size() - 512) {
            for (std::size_t c = 0; c < 24; ++c) {
                ok &= check(expected[(i * 24) + c] == 0, "silent-tail reset completes within file");
            }
        }
    }
    ok &= check(std::ranges::all_of(expected, [](float x) { return std::isfinite(x); }), "finite size/filter PCM");
    for (const std::size_t chunk : {1U, 31U, 32U, 257U, 511U, 512U, 513U, 1024U}) {
        first->reset();
        ok &= check(run(*first, input, chunk) == expected, "reset and arbitrary caller blocks are bit-identical");
    }
    ok &= check(run(*second, input, 31) == expected, "object instances have independent initial state");
    auto other_events = events;
    for (auto& event : other_events) {
        event.position.x = 1 - event.position.x;
        event.position.z = -event.position.z;
    }
    std::vector<float> other_input = input;
    std::ranges::transform(other_input, other_input.begin(), [](float value) { return value * .37F; });
    auto other = SizeObjectProcessor::create(other_events, "22.2", 48000);
    if (!other) {
        return false;
    }
    const auto other_expected = run(*other, other_input, 512);
    first->reset();
    other->reset();
    std::vector<float> first_output;
    std::vector<float> other_output;
    for (std::size_t at = 0; at < input.size(); at += 257) {
        const auto count = std::min<std::size_t>(257, input.size() - at);
        ok &= first->push(std::span<const float>(input).subspan(at, count), first_output).has_value();
        ok &= other->push(std::span<const float>(other_input).subspan(at, count), other_output).has_value();
    }
    ok &= first->finish(first_output).has_value() && other->finish(other_output).has_value();
    ok &= check(first_output == expected && other_output == other_expected,
                "interleaved different objects do not share filters or spatial caches");
    for (const auto mode : {mradm::LfeRoutingMode::direct, mradm::LfeRoutingMode::split_power}) {
        mradm::SceneDirectSpeakersBlock bed;
        bed.speaker_labels = {"RC_LFE"};
        const auto gains = bed_gains(bed, "22.2", mode);
        ok &= check(gains &&
                        std::fabs(std::inner_product(gains->begin(), gains->end(), gains->begin(), 0.0F) - 1) < 1e-6F,
                    "explicit LFE routing conserves single-input power");
        ok &= check(gains && (mode == mradm::LfeRoutingMode::direct ? gains->at(9) == 0 : gains->at(3) == gains->at(9)),
                    "direct and split-power LFE semantics");
    }
    return ok;
}
} // namespace

int main() {
    return geometry_checks() && integral_checks() && state_checks() ? 0 : 1;
}
