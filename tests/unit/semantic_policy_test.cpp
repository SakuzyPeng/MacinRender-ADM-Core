// The semantic policy arithmetic (mradm-dsp `semantic_policy`) against a frozen copy of the C++
// arithmetic it replaced. Parsing, matching and application are Rust since PCM consistency 4.4;
// their behaviour is tested in mradm-scene (`policy.rs`, `policy_semantics.rs`).
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "adm/scene.h"

#include "semantic_policy_ffi.h"

namespace {

bool check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        return false;
    }
    return true;
}

struct GainRule {
    std::optional<float> scale;
    std::optional<float> gain_db;
};

struct PositionRule {
    std::optional<float> azimuth;
    std::optional<float> elevation;
    std::optional<float> distance;
    std::optional<std::array<std::optional<float>, 3>> offset;
    std::optional<float> lock_azimuth;
    std::optional<float> lock_elevation;
};

struct BlockRule {
    std::optional<float> diffuse_scale;
    std::optional<float> diffuse_max;
    float extent_scale{1.0F};
    std::optional<float> width_scale;
    float height_scale{1.0F};
    std::optional<float> depth_scale;
    float extent_max{1.0F};
    std::optional<float> divergence_scale;
    float range_scale{1.0F};
    std::optional<float> max_range;
    PositionRule position;
    uint32_t max_ms{0U};
};

// Frozen copy of the C++ policy arithmetic before it moved to Rust (PCM consistency 1.2).
// The shared transforms must reproduce it bit for bit. The one intended change is the dB
// conversion: ADR 0018 replaces the platform powf with libm's, whose bits are listed here.
namespace original {

float db_to_gain(float db) {
    constexpr std::array<std::pair<float, std::uint32_t>, 7> portable{{{-96.0F, 0x3784F34FU},
                                                                       {-29.845F, 0x3D03DBECU},
                                                                       {-20.5F, 0x3DC157FBU},
                                                                       {-0.1F, 0x3F7D11D1U},
                                                                       {0.0F, 0x3F800000U},
                                                                       {3.3F, 0x3FBB289FU},
                                                                       {18.0F, 0x40FE2F5EU}}};
    // Iterator is a pointer on libc++ and a checked wrapper on MSVC.
    const auto found = // NOLINT(readability-qualified-auto)
        std::ranges::find(portable, db, &std::pair<float, std::uint32_t>::first);
    return found != portable.end() ? std::bit_cast<float>(found->second) : std::nanf("");
}

float gain(float value, const GainRule& policy) {
    if (policy.scale) {
        value *= *policy.scale;
    }
    if (policy.gain_db) {
        value *= db_to_gain(*policy.gain_db);
    }
    return value;
}

bool position_changes(const PositionRule& pos) {
    const auto nonzero = [](const std::optional<float>& v) { return v.has_value() && (*v > 0.0F || *v < 0.0F); };
    const bool offset_changes =
        pos.offset && (nonzero((*pos.offset)[0]) || nonzero((*pos.offset)[1]) || nonzero((*pos.offset)[2]));
    return pos.azimuth || pos.elevation || pos.distance || pos.lock_azimuth || pos.lock_elevation || offset_changes;
}

void polar(float& azimuth, float& elevation, float& distance, const PositionRule& pos) {
    azimuth = pos.azimuth.value_or(azimuth);
    elevation = pos.elevation.value_or(elevation);
    distance = pos.distance.value_or(distance);
    if (pos.offset) {
        azimuth += (*pos.offset)[0].value_or(0.0F);
        elevation += (*pos.offset)[1].value_or(0.0F);
        distance += (*pos.offset)[2].value_or(0.0F);
    }
    azimuth = pos.lock_azimuth.value_or(azimuth);
    elevation = pos.lock_elevation.value_or(elevation);
    azimuth = mradm::wrap_azimuth(azimuth);
    elevation = std::clamp(elevation, -90.0F, 90.0F);
    distance = std::max(0.0F, distance);
}

void block(mradm::SceneObjectBlock& b, const BlockRule& policy, uint32_t sample_rate) {
    b.diffuse *= policy.diffuse_scale.value_or(1.0F);
    b.diffuse = std::clamp(b.diffuse, 0.0F, policy.diffuse_max.value_or(1.0F));
    const float scale = policy.extent_scale;
    b.width *= scale * policy.width_scale.value_or(1.0F);
    b.height *= scale * policy.height_scale;
    b.depth *= scale * policy.depth_scale.value_or(1.0F);
    b.width = std::clamp(b.width, 0.0F, policy.extent_max);
    b.height = std::clamp(b.height, 0.0F, policy.extent_max);
    b.depth = std::clamp(b.depth, 0.0F, policy.extent_max);
    b.divergence = std::clamp(b.divergence * policy.divergence_scale.value_or(1.0F), 0.0F, 1.0F);
    b.divergence_azimuth_range =
        std::clamp(b.divergence_azimuth_range * policy.range_scale, 0.0F, policy.max_range.value_or(120.0F));
    b.divergence_position_range = std::max(0.0F, b.divergence_position_range * policy.range_scale);
    if (position_changes(policy.position)) {
        auto p = mradm::scene_position_to_polar(b.position);
        p.cartesian = false;
        polar(p.azimuth, p.elevation, p.distance, policy.position);
        b.position = p;
    }
    if (policy.max_ms > 0U && b.interp_length_samples) {
        b.interp_length_samples =
            std::min(*b.interp_length_samples, (static_cast<uint64_t>(sample_rate) * policy.max_ms) / 1000U);
    }
}

} // namespace original

// The rules as the policy parser packs them for mradm-dsp.
MradmPolicyValue value(const std::optional<float>& v) {
    return {v.has_value() ? 1U : 0U, v.value_or(0.0F)};
}

MradmPolicyGain gain_rule(const GainRule& gain) {
    return {value(gain.scale), value(gain.gain_db)};
}

MradmPolicyPosition position_rule(const PositionRule& p) {
    MradmPolicyPosition out{};
    out.present = 1U;
    out.azimuth = value(p.azimuth);
    out.elevation = value(p.elevation);
    out.distance = value(p.distance);
    if (p.offset) {
        out.has_offset = 1U;
        out.offset_azimuth = value((*p.offset)[0]);
        out.offset_elevation = value((*p.offset)[1]);
        out.offset_distance = value((*p.offset)[2]);
    }
    out.lock_azimuth = value(p.lock_azimuth);
    out.lock_elevation = value(p.lock_elevation);
    return out;
}

bool apply_gain(const GainRule& gain, std::array<float, 2>& values) {
    const auto rule = gain_rule(gain);
    return mradm_dsp_policy_gain(&rule, values.data(), values.size()) == 0;
}

bool apply_block(mradm::SceneObjectBlock& b, const BlockRule& policy, uint32_t sample_rate) {
    constexpr uint32_t scaled = 2U;
    MradmPolicyObject rule{};
    rule.diffuse = scaled;
    rule.diffuse_scale = value(policy.diffuse_scale);
    rule.diffuse_max = value(policy.diffuse_max);
    rule.extent = scaled;
    rule.extent_scale = value(policy.extent_scale);
    rule.width_scale = value(policy.width_scale);
    rule.height_scale = value(policy.height_scale);
    rule.depth_scale = value(policy.depth_scale);
    rule.extent_max = value(policy.extent_max);
    rule.divergence = scaled;
    rule.divergence_scale = value(policy.divergence_scale);
    rule.divergence_range_scale = value(policy.range_scale);
    rule.divergence_max_range = value(policy.max_range);
    rule.position = position_rule(policy.position);
    rule.interpolation_max_ms = policy.max_ms;
    const auto& p = b.position;
    MradmPolicyBlock numeric{};
    numeric.cartesian = p.cartesian ? 1U : 0U;
    numeric.position[0] = p.cartesian ? p.x : p.azimuth;
    numeric.position[1] = p.cartesian ? p.y : p.elevation;
    numeric.position[2] = p.cartesian ? p.z : p.distance;
    numeric.diffuse = b.diffuse;
    numeric.width = b.width;
    numeric.height = b.height;
    numeric.depth = b.depth;
    numeric.divergence = b.divergence;
    numeric.divergence_azimuth_range = b.divergence_azimuth_range;
    numeric.divergence_position_range = b.divergence_position_range;
    numeric.has_interpolation = b.interp_length_samples.has_value() ? 1U : 0U;
    numeric.interpolation_samples = b.interp_length_samples.value_or(0U);
    if (mradm_dsp_policy_object(&rule, sample_rate, &numeric) != 0) {
        return false;
    }
    if (p.cartesian && numeric.cartesian == 0U) {
        b.position = mradm::SceneBlockPosition{};
    }
    if (numeric.cartesian == 0U) {
        b.position.azimuth = numeric.position[0];
        b.position.elevation = numeric.position[1];
        b.position.distance = numeric.position[2];
    }
    b.diffuse = numeric.diffuse;
    b.width = numeric.width;
    b.height = numeric.height;
    b.depth = numeric.depth;
    b.divergence = numeric.divergence;
    b.divergence_azimuth_range = numeric.divergence_azimuth_range;
    b.divergence_position_range = numeric.divergence_position_range;
    if (b.interp_length_samples.has_value()) {
        b.interp_length_samples = numeric.interpolation_samples;
    }
    return true;
}

bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool same_block(const mradm::SceneObjectBlock& a, const mradm::SceneObjectBlock& b) {
    const auto& p = a.position;
    const auto& q = b.position;
    return p.cartesian == q.cartesian && same_bits(p.azimuth, q.azimuth) && same_bits(p.elevation, q.elevation) &&
           same_bits(p.distance, q.distance) && same_bits(p.x, q.x) && same_bits(p.y, q.y) && same_bits(p.z, q.z) &&
           same_bits(a.diffuse, b.diffuse) && same_bits(a.width, b.width) && same_bits(a.height, b.height) &&
           same_bits(a.depth, b.depth) && same_bits(a.divergence, b.divergence) &&
           same_bits(a.divergence_azimuth_range, b.divergence_azimuth_range) &&
           same_bits(a.divergence_position_range, b.divergence_position_range) &&
           a.interp_length_samples == b.interp_length_samples;
}

// NOLINTNEXTLINE(readability-function-size)
bool verify_numeric_matches_original() {
    bool ok = true;
    const std::array<std::optional<float>, 5> scales{std::nullopt, 0.0F, 0.37F, 1.0F, 2.75F};
    const std::array<std::optional<float>, 8> decibels{
        std::nullopt, -96.0F, -29.845F, -20.5F, -0.1F, 0.0F, 3.3F, 18.0F};
    std::size_t cases = 0;
    for (const auto& scale : scales) {
        for (const auto& gain_db : decibels) {
            const GainRule gain{scale, gain_db};
            std::array object{0.83F, 1.2F};
            ok &= check(apply_gain(gain, object) && same_bits(object[0], original::gain(0.83F, gain)) &&
                            same_bits(object[1], original::gain(1.2F, gain)),
                        "numeric: object gain matches the original arithmetic");

            MradmPolicyDirect rule{};
            rule.gain = gain_rule(gain);
            MradmPolicyDirectBlock ds{0.61F, 1.0F, 0U, {0.0F, 0.0F, 1.0F}, 0U};
            ok &= check(mradm_dsp_policy_direct(&rule, &ds) == 0 && same_bits(ds.gain, original::gain(0.61F, gain)) &&
                            same_bits(ds.user_gain_multiplier, original::gain(1.0F, gain)),
                        "numeric: DirectSpeakers gain matches the original arithmetic");
            ++cases;
        }
    }

    const std::array<std::optional<float>, 4> offsets{std::nullopt, -0.0F, -47.25F, 361.5F};
    const std::array<mradm::SceneBlockPosition, 3> positions{
        mradm::SceneBlockPosition{true, 0.0F, 0.0F, 1.0F, -0.3F, 0.8F, 0.25F},
        mradm::SceneBlockPosition{true, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -0.0F},
        mradm::SceneBlockPosition{false, -179.5F, 80.0F, 0.6F, 0.0F, 0.0F, 0.0F}};
    for (const auto& start : positions) {
        for (const auto& offset : offsets) {
            for (const auto& absolute : offsets) {
                PositionRule position;
                position.azimuth = absolute;
                position.offset = std::array{offset, offset, offset};
                if (absolute && *absolute > 300.0F) {
                    position.lock_elevation = -100.0F;
                }
                // Validated policies reject negative scales and limits; -0.0 passes validation.
                const auto magnitude = [](std::optional<float> v) { return v && *v < 0.0F ? -*v : v; };
                BlockRule policy;
                policy.diffuse_scale = magnitude(absolute);
                policy.diffuse_max = magnitude(offset);
                policy.extent_scale = 1.5F;
                policy.width_scale = magnitude(offset);
                policy.height_scale = 0.3F;
                policy.depth_scale = magnitude(absolute);
                policy.extent_max = 0.9F;
                policy.divergence_scale = magnitude(offset);
                policy.range_scale = 1.9F;
                policy.max_range = magnitude(absolute);
                policy.position = position;
                policy.max_ms = 7U;

                mradm::SceneObjectBlock block;
                block.position = start;
                block.diffuse = 0.7F;
                block.width = 0.4F;
                block.height = 0.2F;
                block.depth = 0.9F;
                block.divergence = 0.6F;
                block.divergence_azimuth_range = 70.0F;
                block.divergence_position_range = 0.35F;
                block.interp_length_samples = 1000U;
                auto expected = block;
                original::block(expected, policy, 44100U);
                ok &= check(apply_block(block, policy, 44100U) && same_block(block, expected),
                            "numeric: Objects block matches the original arithmetic");

                MradmPolicyDirect rule{};
                rule.position = position_rule(position);
                MradmPolicyDirectBlock ds{1.0F, 1.0F, 0U, {start.azimuth, start.elevation, start.distance}, 0U};
                float az = start.azimuth;
                float el = start.elevation;
                float dist = start.distance;
                const bool moves = original::position_changes(position);
                if (moves) {
                    original::polar(az, el, dist, position);
                }
                ok &= check(mradm_dsp_policy_direct(&rule, &ds) == 0 && (ds.has_position != 0U) == moves &&
                                (ds.user_position_override != 0U) == moves && same_bits(ds.polar[0], az) &&
                                same_bits(ds.polar[1], el) && same_bits(ds.polar[2], dist),
                            "numeric: DirectSpeakers re-aim matches the original arithmetic");
                ++cases;
            }
        }
    }
    ok &= check(cases == 88U, "numeric: every grid case ran");
    return ok;
}

} // namespace

int main() {
    if (verify_numeric_matches_original()) {
        std::cout << "semantic policy test passed\n";
        return 0;
    }
    return 1;
}
