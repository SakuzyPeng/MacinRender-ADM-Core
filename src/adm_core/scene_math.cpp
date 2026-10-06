#include "scene_math.h"

#include <array>
#include <limits>

#include "adm/scene.h"
namespace mradm {
SceneBlockPosition apply_position_offset(SceneBlockPosition pos, const ScenePositionOffset& off) {
    if (pos.cartesian != off.cartesian) {
        return pos;
    }
    const auto input =
        pos.cartesian ? std::array{pos.x, pos.y, pos.z, off.x, off.y, off.z}
                      : std::array{pos.azimuth, pos.elevation, pos.distance, off.azimuth, off.elevation, off.distance};
    const auto value = dsp::scene_math<6, 3>(pos.cartesian ? 9U : 8U, input);
    if (pos.cartesian) {
        pos.x = value[0];
        pos.y = value[1];
        pos.z = value[2];
    } else {
        pos.azimuth = value[0];
        pos.elevation = value[1];
        pos.distance = value[2];
    }
    return pos;
}
float wrap_azimuth(float azimuth) {
    return dsp::scene_math<1, 1>(4U, {azimuth})[0];
}
SceneBlockPosition scene_position_to_polar(const SceneBlockPosition& pos) {
    if (!pos.cartesian) {
        return pos;
    }
    const auto p = dsp::scene_math<3, 3>(0U, {pos.x, pos.y, pos.z});
    SceneBlockPosition result;
    result.cartesian = false;
    result.azimuth = p[0];
    result.elevation = p[1];
    result.distance = p[2];
    return result;
}
SceneDirectionVector direction_vector_from_polar(float azimuth, float elevation) {
    const auto v = dsp::scene_math<2, 3>(1U, {azimuth, elevation});
    return {v[0], v[1], v[2]};
}
SceneDirectionVector direction_vector_from_position(const SceneBlockPosition& pos) {
    const auto p = scene_position_to_polar(pos);
    return direction_vector_from_polar(p.azimuth, p.elevation);
}
float direction_distance(const SceneDirectionVector& lhs, const SceneDirectionVector& rhs) {
    return dsp::scene_math<6, 1>(3U, {lhs.x, lhs.y, lhs.z, rhs.x, rhs.y, rhs.z})[0];
}
std::optional<size_t> nearest_non_lfe_speaker_index(const SceneBlockPosition& pos,
                                                    const std::vector<SceneOutputSpeaker>& speakers) {
    std::array<MradmSceneSpeaker, 32> numeric{};
    const auto input =
        pos.cartesian ? std::array{pos.x, pos.y, pos.z} : std::array{pos.azimuth, pos.elevation, pos.distance};
    size_t index = std::numeric_limits<size_t>::max();
    float best = std::numeric_limits<float>::max();
    for (size_t offset = 0; offset < speakers.size();) {
        const size_t count = std::min(numeric.size(), speakers.size() - offset);
        for (size_t i = 0; i < count; ++i) {
            const auto& speaker = speakers[offset + i];
            numeric.at(i) = {speaker.azimuth, speaker.elevation, speaker.is_lfe ? 1U : 0U};
        }
        size_t candidate = std::numeric_limits<size_t>::max();
        float distance = std::numeric_limits<float>::max();
        dsp::scene_check(mradm_dsp_scene_nearest(input.data(),
                                                 input.size(),
                                                 (pos.cartesian ? 1U : 0U) + dsp::scene_cpp_contract,
                                                 numeric.data(),
                                                 count,
                                                 &candidate,
                                                 &distance));
        if (distance < best) {
            best = distance;
            index = offset + candidate;
        }
        offset += count;
    }
    return index == std::numeric_limits<size_t>::max() ? std::nullopt : std::optional<size_t>{index};
}
SceneObjectBlock apply_channel_lock(const SceneObjectBlock& block, const std::vector<SceneOutputSpeaker>& speakers) {
    if (!block.channel_lock) {
        return block;
    }
    const auto index = nearest_non_lfe_speaker_index(block.position, speakers);
    if (!index) {
        return block;
    }
    const auto& speaker = speakers[*index];
    const auto src = direction_vector_from_position(block.position);
    const auto spk = direction_vector_from_polar(speaker.azimuth, speaker.elevation);
    if (block.channel_lock_max_distance &&
        direction_distance(src, spk) > (*block.channel_lock_max_distance + 1.0e-4F)) {
        return block;
    }
    SceneObjectBlock result = block;
    result.position.cartesian = false;
    result.position.distance = scene_position_to_polar(block.position).distance;
    result.position.azimuth = speaker.azimuth;
    result.position.elevation = speaker.elevation;
    return result;
}
std::vector<SceneObjectBlock> expand_object_divergence(const SceneObjectBlock& block) {
    // Preserve the unconverted original position for a single source.
    if (std::clamp(block.divergence, 0.0F, 1.0F) <= 1.0e-4F) {
        return {block};
    }
    const auto& p = block.position;
    const std::array input{p.cartesian ? p.x : p.azimuth,
                           p.cartesian ? p.y : p.elevation,
                           p.cartesian ? p.z : p.distance,
                           block.divergence,
                           block.divergence_azimuth_range,
                           block.divergence_position_range,
                           block.gain};
    std::array<MradmSceneCloudPoint, 3> points{};
    size_t count = 0;
    dsp::scene_check(mradm_dsp_scene_divergence(
        input.data(), input.size(), p.cartesian ? 1U : 0U, points.data(), points.size(), &count));
    SceneObjectBlock base = block;
    base.position = scene_position_to_polar(p);
    std::vector<SceneObjectBlock> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto source = base;
        source.position.azimuth = points.at(i).azimuth;
        source.position.elevation = points.at(i).elevation;
        source.gain = points.at(i).weight;
        result.push_back(source);
    }
    return result;
}
} // namespace mradm
