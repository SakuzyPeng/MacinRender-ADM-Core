#include "adm/semantic_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "semantic_policy_ffi.h"

namespace mradm {
namespace {

using Json = nlohmann::json;

constexpr std::string_view k_diffuse_key = "diffuse";
constexpr std::string_view k_extent_key = "extent";
constexpr std::string_view k_divergence_key = "divergence";
constexpr std::string_view k_channel_lock_key = "channel_lock";
constexpr std::string_view k_interpolation_key = "interpolation";
constexpr std::string_view k_gain_key = "gain";
constexpr std::string_view k_position_key = "position";
constexpr std::string_view k_direct_speakers_key = "direct_speakers";
constexpr std::string_view k_head_locked_key = "head_locked";

[[nodiscard]] Error invalid_policy(const std::string& path, const std::string& message) {
    return {ErrorCode::invalid_argument, "invalid semantic policy: " + message, path};
}

[[nodiscard]] bool
has_unknown_keys(const Json& obj, std::initializer_list<std::string_view> allowed, std::string& unknown) {
    if (!obj.is_object()) {
        return false;
    }
    for (const auto& [key, value] : obj.items()) {
        (void) value;
        const bool is_allowed =
            std::ranges::any_of(allowed, [&](std::string_view allowed_key) { return allowed_key == key; });
        if (!is_allowed) {
            unknown = key;
            return true;
        }
    }
    return false;
}

template <typename T>
[[nodiscard]] Result<T> read_number(const Json& obj, std::string_view key, const std::string& path) {
    const auto it = obj.find(std::string{key});
    if (it == obj.end()) {
        return make_error(ErrorCode::internal_error, "missing JSON field", std::string{key});
    }
    if constexpr (std::is_unsigned_v<T>) {
        if (!it->is_number_unsigned()) {
            return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be an unsigned integer", key))};
        }
    } else if (!it->is_number()) {
        return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be numeric", key))};
    }
    T value{};
    try {
        value = it->get<T>();
    } catch (const Json::exception& e) {
        return tl::unexpected{invalid_policy(path, fmt::format("'{}' has invalid numeric value: {}", key, e.what()))};
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) {
            return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be finite", key))};
        }
    }
    return value;
}

[[nodiscard]] Result<bool> read_bool(const Json& obj, std::string_view key, const std::string& path) {
    const auto it = obj.find(std::string{key});
    if (it == obj.end()) {
        return make_error(ErrorCode::internal_error, "missing JSON field", std::string{key});
    }
    if (!it->is_boolean()) {
        return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be boolean", key))};
    }
    return it->get<bool>();
}

[[nodiscard]] Result<std::string> read_string(const Json& obj, std::string_view key, const std::string& path) {
    const auto it = obj.find(std::string{key});
    if (it == obj.end()) {
        return make_error(ErrorCode::internal_error, "missing JSON field", std::string{key});
    }
    if (!it->is_string()) {
        return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be a string", key))};
    }
    return it->get<std::string>();
}

[[nodiscard]] Result<DiffusePolicy> parse_diffuse(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'diffuse' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"enabled", "scale", "max"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown diffuse field '" + unknown + "'")};
    }

    DiffusePolicy out;
    if (obj.contains("enabled")) {
        auto value = read_bool(obj, "enabled", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.enabled = *value;
    }
    for (const auto key : {std::string_view{"scale"}, std::string_view{"max"}}) {
        if (!obj.contains(std::string{key})) {
            continue;
        }
        auto value = read_number<float>(obj, key, path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        if (*value < 0.0F) {
            return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be >= 0", key))};
        }
        if (key == "scale") {
            out.scale = *value;
        } else {
            out.max = *value;
        }
    }
    return out;
}

[[nodiscard]] Result<ExtentPolicy> parse_extent(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'extent' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"enabled", "scale", "width_scale", "height_scale", "depth_scale", "max"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown extent field '" + unknown + "'")};
    }

    ExtentPolicy out;
    if (obj.contains("enabled")) {
        auto value = read_bool(obj, "enabled", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.enabled = *value;
    }
    for (const auto key : {std::string_view{"scale"},
                           std::string_view{"width_scale"},
                           std::string_view{"height_scale"},
                           std::string_view{"depth_scale"},
                           std::string_view{"max"}}) {
        if (!obj.contains(std::string{key})) {
            continue;
        }
        auto value = read_number<float>(obj, key, path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        if (*value < 0.0F) {
            return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be >= 0", key))};
        }
        if (key == "scale") {
            out.scale = *value;
        } else if (key == "width_scale") {
            out.width_scale = *value;
        } else if (key == "height_scale") {
            out.height_scale = *value;
        } else if (key == "depth_scale") {
            out.depth_scale = *value;
        } else {
            out.max = *value;
        }
    }
    return out;
}

[[nodiscard]] Result<DivergencePolicy> parse_divergence(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'divergence' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"enabled", "scale", "range_scale", "max_range"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown divergence field '" + unknown + "'")};
    }

    DivergencePolicy out;
    if (obj.contains("enabled")) {
        auto value = read_bool(obj, "enabled", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.enabled = *value;
    }
    for (const auto key : {std::string_view{"scale"}, std::string_view{"range_scale"}, std::string_view{"max_range"}}) {
        if (!obj.contains(std::string{key})) {
            continue;
        }
        auto value = read_number<float>(obj, key, path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        if (*value < 0.0F) {
            return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be >= 0", key))};
        }
        if (key == "scale") {
            out.scale = *value;
        } else if (key == "range_scale") {
            out.range_scale = *value;
        } else {
            out.max_range = *value;
        }
    }
    return out;
}

[[nodiscard]] Result<ChannelLockPolicy> parse_channel_lock(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'channel_lock' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"enabled", "max_distance"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown channel_lock field '" + unknown + "'")};
    }
    ChannelLockPolicy out;
    if (obj.contains("enabled")) {
        auto value = read_bool(obj, "enabled", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.enabled = *value;
    }
    if (obj.contains("max_distance")) {
        auto value = read_number<float>(obj, "max_distance", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        if (*value < 0.0F) {
            return tl::unexpected{invalid_policy(path, "'max_distance' must be >= 0")};
        }
        out.max_distance = *value;
    }
    return out;
}

[[nodiscard]] Result<InterpolationPolicy> parse_interpolation(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'interpolation' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"honor_jump_position", "max_ms"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown interpolation field '" + unknown + "'")};
    }
    InterpolationPolicy out;
    if (obj.contains("honor_jump_position")) {
        auto value = read_bool(obj, "honor_jump_position", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.honor_jump_position = *value;
    }
    if (obj.contains("max_ms")) {
        auto value = read_number<uint32_t>(obj, "max_ms", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.max_ms = *value;
    }
    return out;
}

[[nodiscard]] Result<GainPolicy> parse_gain(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'gain' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"scale", "gain_db", "mute"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown gain field '" + unknown + "'")};
    }
    GainPolicy out;
    if (obj.contains("scale")) {
        auto value = read_number<float>(obj, "scale", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        if (*value < 0.0F) {
            return tl::unexpected{invalid_policy(path, "'scale' must be >= 0")};
        }
        out.scale = *value;
    }
    if (obj.contains("gain_db")) {
        auto value = read_number<float>(obj, "gain_db", path); // any finite dB offset
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.gain_db = *value;
    }
    if (obj.contains("mute")) {
        auto value = read_bool(obj, "mute", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.mute = *value;
    }
    return out;
}

[[nodiscard]] Result<PositionPolicy> parse_position(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'position' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(
            obj, {"azimuth", "elevation", "distance", "offset", "lock_azimuth", "lock_elevation"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown position field '" + unknown + "'")};
    }

    // Read an optional finite float; `non_negative` enforces distance-style fields.
    const auto read_opt = [&](std::string_view key, bool non_negative, std::optional<float>& dst) -> Result<void> {
        if (!obj.contains(std::string{key})) {
            return {};
        }
        auto value = read_number<float>(obj, key, path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        if (non_negative && *value < 0.0F) {
            return tl::unexpected{invalid_policy(path, fmt::format("'{}' must be >= 0", key))};
        }
        dst = *value;
        return {};
    };

    PositionPolicy out;
    if (auto r = read_opt("azimuth", false, out.azimuth); !r) {
        return tl::unexpected{r.error()};
    }
    if (auto r = read_opt("elevation", false, out.elevation); !r) {
        return tl::unexpected{r.error()};
    }
    if (auto r = read_opt("distance", true, out.distance); !r) {
        return tl::unexpected{r.error()};
    }
    if (auto r = read_opt("lock_azimuth", false, out.lock_azimuth); !r) {
        return tl::unexpected{r.error()};
    }
    if (auto r = read_opt("lock_elevation", false, out.lock_elevation); !r) {
        return tl::unexpected{r.error()};
    }
    if (obj.contains("offset")) {
        const auto& off_json = obj.at("offset");
        if (!off_json.is_object()) {
            return tl::unexpected{invalid_policy(path, "'offset' must be an object")};
        }
        std::string off_unknown;
        if (has_unknown_keys(off_json, {"azimuth", "elevation", "distance"}, off_unknown)) {
            return tl::unexpected{invalid_policy(path, "unknown offset field '" + off_unknown + "'")};
        }
        PositionPolicy::Offset offset;
        const auto read_off = [&](std::string_view key, std::optional<float>& dst) -> Result<void> {
            if (!off_json.contains(std::string{key})) {
                return {};
            }
            auto value = read_number<float>(off_json, key, path); // offsets may be negative
            if (!value) {
                return tl::unexpected{value.error()};
            }
            dst = *value;
            return {};
        };
        if (auto r = read_off("azimuth", offset.azimuth); !r) {
            return tl::unexpected{r.error()};
        }
        if (auto r = read_off("elevation", offset.elevation); !r) {
            return tl::unexpected{r.error()};
        }
        if (auto r = read_off("distance", offset.distance); !r) {
            return tl::unexpected{r.error()};
        }
        out.offset = offset;
    }
    return out;
}

[[nodiscard]] Result<DirectSpeakersPolicy> parse_direct_speakers(const Json& obj, const std::string& path) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "'direct_speakers' must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(obj, {"speaker_label", "lfe", "head_locked", "gain", "position"}, unknown)) {
        return tl::unexpected{invalid_policy(path, "unknown direct_speakers field '" + unknown + "'")};
    }
    DirectSpeakersPolicy out;
    if (obj.contains("speaker_label")) {
        auto value = read_string(obj, "speaker_label", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.speaker_label = *value;
    }
    if (obj.contains("lfe")) {
        auto value = read_bool(obj, "lfe", path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.lfe = *value;
    }
    if (obj.contains(std::string{k_head_locked_key})) {
        auto value = read_bool(obj, k_head_locked_key, path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.head_locked = *value;
    }
    if (obj.contains("gain")) {
        auto value = parse_gain(obj.at("gain"), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.gain = *value;
    }
    if (obj.contains("position")) {
        auto value = parse_position(obj.at("position"), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.position = *value;
    }
    return out;
}

[[nodiscard]] Result<SemanticPolicyOverride>
parse_override(const Json& obj, const std::string& path, std::initializer_list<std::string_view> extra_keys = {}) {
    if (!obj.is_object()) {
        return tl::unexpected{invalid_policy(path, "policy section must be an object")};
    }

    std::vector<std::string_view> allowed{k_diffuse_key,
                                          k_extent_key,
                                          k_divergence_key,
                                          k_channel_lock_key,
                                          k_interpolation_key,
                                          k_gain_key,
                                          k_position_key,
                                          k_direct_speakers_key,
                                          k_head_locked_key};
    allowed.insert(allowed.end(), extra_keys.begin(), extra_keys.end());
    for (const auto& [key, value] : obj.items()) {
        (void) value;
        if (std::ranges::find(allowed, std::string_view{key}) == allowed.end()) {
            return tl::unexpected{invalid_policy(path, "unknown policy field '" + key + "'")};
        }
    }

    SemanticPolicyOverride out;
    if (obj.contains(std::string{k_head_locked_key})) {
        auto value = read_bool(obj, k_head_locked_key, path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.head_locked = *value;
    }
    if (obj.contains(std::string{k_diffuse_key})) {
        auto value = parse_diffuse(obj.at(std::string{k_diffuse_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.diffuse = *value;
    }
    if (obj.contains(std::string{k_extent_key})) {
        auto value = parse_extent(obj.at(std::string{k_extent_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.extent = *value;
    }
    if (obj.contains(std::string{k_divergence_key})) {
        auto value = parse_divergence(obj.at(std::string{k_divergence_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.divergence = *value;
    }
    if (obj.contains(std::string{k_channel_lock_key})) {
        auto value = parse_channel_lock(obj.at(std::string{k_channel_lock_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.channel_lock = *value;
    }
    if (obj.contains(std::string{k_interpolation_key})) {
        auto value = parse_interpolation(obj.at(std::string{k_interpolation_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.interpolation = *value;
    }
    if (obj.contains(std::string{k_gain_key})) {
        auto value = parse_gain(obj.at(std::string{k_gain_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.gain = *value;
    }
    if (obj.contains(std::string{k_position_key})) {
        auto value = parse_position(obj.at(std::string{k_position_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.position = *value;
    }
    if (obj.contains(std::string{k_direct_speakers_key})) {
        auto value = parse_direct_speakers(obj.at(std::string{k_direct_speakers_key}), path);
        if (!value) {
            return tl::unexpected{value.error()};
        }
        out.direct_speakers = *value;
    }
    return out;
}

template <typename T> void merge_optional(std::optional<T>& dst, const std::optional<T>& src) {
    if (src.has_value()) {
        dst = src;
    }
}

void merge_policy(DiffusePolicy& dst, const DiffusePolicy& src) {
    merge_optional(dst.enabled, src.enabled);
    merge_optional(dst.scale, src.scale);
    merge_optional(dst.max, src.max);
}

void merge_policy(ExtentPolicy& dst, const ExtentPolicy& src) {
    merge_optional(dst.enabled, src.enabled);
    merge_optional(dst.scale, src.scale);
    merge_optional(dst.width_scale, src.width_scale);
    merge_optional(dst.height_scale, src.height_scale);
    merge_optional(dst.depth_scale, src.depth_scale);
    merge_optional(dst.max, src.max);
}

void merge_policy(DivergencePolicy& dst, const DivergencePolicy& src) {
    merge_optional(dst.enabled, src.enabled);
    merge_optional(dst.scale, src.scale);
    merge_optional(dst.range_scale, src.range_scale);
    merge_optional(dst.max_range, src.max_range);
}

void merge_policy(ChannelLockPolicy& dst, const ChannelLockPolicy& src) {
    merge_optional(dst.enabled, src.enabled);
    merge_optional(dst.max_distance, src.max_distance);
}

void merge_policy(GainPolicy& dst, const GainPolicy& src) {
    merge_optional(dst.scale, src.scale);
    merge_optional(dst.gain_db, src.gain_db);
    merge_optional(dst.mute, src.mute);
}

void merge_policy(PositionPolicy& dst, const PositionPolicy& src) {
    merge_optional(dst.azimuth, src.azimuth);
    merge_optional(dst.elevation, src.elevation);
    merge_optional(dst.distance, src.distance);
    merge_optional(dst.lock_azimuth, src.lock_azimuth);
    merge_optional(dst.lock_elevation, src.lock_elevation);
    if (src.offset) {
        if (!dst.offset) {
            dst.offset = PositionPolicy::Offset{};
        }
        merge_optional(dst.offset->azimuth, src.offset->azimuth);
        merge_optional(dst.offset->elevation, src.offset->elevation);
        merge_optional(dst.offset->distance, src.offset->distance);
    }
}

// NOTE: DirectSpeakersPolicy is deliberately NOT mergeable. Each DS rule pairs a
// per-block filter (speaker_label / lfe) with an action; merging two rules would
// fuse their filters (e.g. "M+000" AND "lfe") and break both. DS policies are
// instead collected into an ordered list and applied independently per ds_block
// (see apply_semantic_policy).

void merge_policy(InterpolationPolicy& dst, const InterpolationPolicy& src) {
    merge_optional(dst.honor_jump_position, src.honor_jump_position);
    merge_optional(dst.max_ms, src.max_ms);
}

void merge_override(SemanticPolicyOverride& dst, const SemanticPolicyOverride& src) {
    merge_optional(dst.head_locked, src.head_locked);
    if (src.diffuse) {
        if (!dst.diffuse) {
            dst.diffuse = DiffusePolicy{};
        }
        merge_policy(*dst.diffuse, *src.diffuse);
    }
    if (src.extent) {
        if (!dst.extent) {
            dst.extent = ExtentPolicy{};
        }
        merge_policy(*dst.extent, *src.extent);
    }
    if (src.divergence) {
        if (!dst.divergence) {
            dst.divergence = DivergencePolicy{};
        }
        merge_policy(*dst.divergence, *src.divergence);
    }
    if (src.channel_lock) {
        if (!dst.channel_lock) {
            dst.channel_lock = ChannelLockPolicy{};
        }
        merge_policy(*dst.channel_lock, *src.channel_lock);
    }
    if (src.interpolation) {
        if (!dst.interpolation) {
            dst.interpolation = InterpolationPolicy{};
        }
        merge_policy(*dst.interpolation, *src.interpolation);
    }
    if (src.gain) {
        if (!dst.gain) {
            dst.gain = GainPolicy{};
        }
        merge_policy(*dst.gain, *src.gain);
    }
    if (src.position) {
        if (!dst.position) {
            dst.position = PositionPolicy{};
        }
        merge_policy(*dst.position, *src.position);
    }
    // direct_speakers is intentionally not merged here (see note above); it is
    // collected and applied per-rule by apply_semantic_policy.
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string out{text};
    std::ranges::transform(
        out, out.begin(), [](char c) { return (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c); });
    return out;
}

[[nodiscard]] bool glob_match_lower(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
        return false;
    }
    const auto pat = lower_ascii(pattern);
    const auto str = lower_ascii(text);
    std::size_t p = 0;
    std::size_t s = 0;
    std::size_t star = std::string::npos;
    std::size_t match = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p;
            ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            match = s;
        } else if (star != std::string::npos) {
            p = star + 1;
            s = ++match;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') {
        ++p;
    }
    return p == pat.size();
}

[[nodiscard]] bool ascii_equal_ignore_case(std::string_view lhs, std::string_view rhs) {
    return lower_ascii(lhs) == lower_ascii(rhs);
}

// Content / programme membership for one object, resolved from the scene graph.
struct ObjectMembership {
    std::vector<std::string> content_ids;
    std::vector<std::string> content_names;
    std::vector<std::string> programme_ids;
    std::vector<std::string> programme_names;
};

// Precompute object_id -> membership by walking scene.contents (object_ids) and
// scene.programmes (content_ids -> contents -> object_ids).
[[nodiscard]] std::unordered_map<std::string, ObjectMembership> build_membership(const AdmScene& scene) {
    std::unordered_map<std::string, ObjectMembership> out;
    std::unordered_map<std::string, const SceneContent*> content_by_id;
    for (const auto& content : scene.contents) {
        content_by_id.emplace(content.id, &content);
        for (const auto& oid : content.object_ids) {
            auto& m = out[oid];
            m.content_ids.push_back(content.id);
            m.content_names.push_back(content.name);
        }
    }
    for (const auto& programme : scene.programmes) {
        for (const auto& cid : programme.content_ids) {
            const auto it = content_by_id.find(cid);
            if (it == content_by_id.end()) {
                continue;
            }
            for (const auto& oid : it->second->object_ids) {
                auto& m = out[oid];
                m.programme_ids.push_back(programme.id);
                m.programme_names.push_back(programme.name);
            }
        }
    }
    return out;
}

[[nodiscard]] bool any_equal_ignore_case(const std::vector<std::string>& values, std::string_view target) {
    return std::ranges::any_of(values, [&](const std::string& v) { return ascii_equal_ignore_case(v, target); });
}

[[nodiscard]] bool
rule_matches(const SceneObject& object, const ObjectMembership& membership, const SemanticObjectRule& rule) {
    if (rule.all.value_or(false)) {
        return true;
    }
    if (!rule.id.empty() && ascii_equal_ignore_case(object.id, rule.id)) {
        return true;
    }
    if (!rule.name.empty() && ascii_equal_ignore_case(object.name, rule.name)) {
        return true;
    }
    if (!rule.name_glob.empty() && glob_match_lower(rule.name_glob, object.name)) {
        return true;
    }
    if (!rule.track_uid.empty() && std::ranges::any_of(object.tracks, [&](const SceneTrackRef& track) {
            return ascii_equal_ignore_case(track.track_uid, rule.track_uid);
        })) {
        return true;
    }
    if ((rule.importance_min || rule.importance_max) && object.importance.has_value()) {
        const int imp = *object.importance;
        if (imp >= rule.importance_min.value_or(std::numeric_limits<int>::min()) &&
            imp <= rule.importance_max.value_or(std::numeric_limits<int>::max())) {
            return true;
        }
    }
    if (rule.dialogue_id && object.dialogue_id.has_value() && std::cmp_equal(*object.dialogue_id, *rule.dialogue_id)) {
        return true;
    }
    if (!rule.content.empty() && (any_equal_ignore_case(membership.content_ids, rule.content) ||
                                  any_equal_ignore_case(membership.content_names, rule.content))) {
        return true;
    }
    if (!rule.programme.empty() && (any_equal_ignore_case(membership.programme_ids, rule.programme) ||
                                    any_equal_ignore_case(membership.programme_names, rule.programme))) {
        return true;
    }
    return false;
}

// Policy arithmetic lives in Rust (mradm-dsp `semantic_policy`); these helpers only pack the
// optional parameters and copy block fields. Booleans and filters stay here.
void policy_check(int status) noexcept {
    if (status != 0) {
        std::terminate();
    }
}

[[nodiscard]] MradmPolicyValue policy_value(const std::optional<float>& value) {
    return {value.has_value() ? 1U : 0U, value.value_or(0.0F)};
}

[[nodiscard]] MradmPolicyGain policy_gain(const GainPolicy& gain) {
    return {policy_value(gain.scale), policy_value(gain.gain_db)};
}

[[nodiscard]] MradmPolicyPosition policy_position(const std::optional<PositionPolicy>& position) {
    MradmPolicyPosition out{};
    if (!position) {
        return out;
    }
    out.present = 1U;
    out.azimuth = policy_value(position->azimuth);
    out.elevation = policy_value(position->elevation);
    out.distance = policy_value(position->distance);
    if (position->offset) {
        out.has_offset = 1U;
        out.offset_azimuth = policy_value(position->offset->azimuth);
        out.offset_elevation = policy_value(position->offset->elevation);
        out.offset_distance = policy_value(position->offset->distance);
    }
    out.lock_azimuth = policy_value(position->lock_azimuth);
    out.lock_elevation = policy_value(position->lock_elevation);
    return out;
}

template <typename Section> [[nodiscard]] std::uint32_t policy_section(const std::optional<Section>& section) {
    constexpr std::uint32_t disabled = 1U;
    constexpr std::uint32_t scaled = 2U;
    if (!section) {
        return 0U;
    }
    return section->enabled.has_value() && !*section->enabled ? disabled : scaled;
}

void apply_gain_policy(const GainPolicy& gain, std::span<float> values) {
    const auto rule = policy_gain(gain);
    policy_check(mradm_dsp_policy_gain(&rule, values.data(), values.size()));
}

// Object-level (per AudioObject) application: gain + mute. Called once per object.
void apply_object_override(SceneObject& object, const SemanticPolicyOverride& policy) {
    if (policy.gain) {
        std::array values{object.gain, object.user_level.gain_multiplier};
        apply_gain_policy(*policy.gain, values);
        object.gain = values[0];
        object.user_level.gain_multiplier = values[1];
        if (policy.gain->mute.has_value()) {
            object.mute = *policy.gain->mute;
            object.user_level.mute = policy.gain->mute;
        }
    }
}

// HOA packs live outside scene.objects and are matched by object_id / pack_format
// / all. Gain/mute apply at pack level; headLocked applies to every channel block.
[[nodiscard]] bool hoa_rule_matches(const SceneHOATracks& pack, const SemanticObjectRule& rule) {
    if (rule.all.value_or(false)) {
        return true;
    }
    if (!rule.id.empty() && ascii_equal_ignore_case(pack.object_id, rule.id)) {
        return true;
    }
    if (!rule.pack_format.empty() && ascii_equal_ignore_case(pack.pack_format_id, rule.pack_format)) {
        return true;
    }
    return false;
}

// Effective gain for a HOA pack: global gain first, then each matching rule's gain
// (last-write-wins merge; gain has no per-block filter so merging is safe).
[[nodiscard]] std::optional<GainPolicy> effective_hoa_gain(const SceneHOATracks& pack, const SemanticPolicy& policy) {
    std::optional<GainPolicy> out;
    const auto fold = [&](const GainPolicy& g) {
        if (!out) {
            out = GainPolicy{};
        }
        merge_policy(*out, g);
    };
    if (policy.global && policy.global->gain) {
        fold(*policy.global->gain);
    }
    for (const auto& rule : policy.objects) {
        if (rule.gain && hoa_rule_matches(pack, rule)) {
            fold(*rule.gain);
        }
    }
    return out;
}

void apply_hoa_gain(SceneHOATracks& pack, const GainPolicy& gain) {
    if (gain.mute.has_value()) {
        pack.mute = *gain.mute;
    }
    apply_gain_policy(gain, std::span{&pack.gain, 1U});
}

[[nodiscard]] std::optional<bool> effective_hoa_head_locked(const SceneHOATracks& pack, const SemanticPolicy& policy) {
    std::optional<bool> out;
    if (policy.global && policy.global->head_locked.has_value()) {
        out = policy.global->head_locked;
    }
    for (const auto& rule : policy.objects) {
        if (rule.head_locked.has_value() && hoa_rule_matches(pack, rule)) {
            out = rule.head_locked;
        }
    }
    return out;
}

// DirectSpeakers block override: filter by speaker_label / lfe (AND), then apply
// gain (scale/gain_db; mute -> ds.gain = 0) and position re-aim.
void apply_ds_override(SceneDirectSpeakersBlock& ds, const DirectSpeakersPolicy& policy) {
    if (!policy.speaker_label.empty() && !std::ranges::any_of(ds.speaker_labels, [&](const std::string& label) {
            return ascii_equal_ignore_case(label, policy.speaker_label);
        })) {
        return;
    }
    if (policy.lfe.has_value() && *policy.lfe != ds.low_pass_hz.has_value()) {
        return;
    }
    if (policy.head_locked.has_value()) {
        ds.head_locked = *policy.head_locked;
    }
    MradmPolicyDirect rule{};
    if (policy.gain) {
        rule.gain = policy_gain(*policy.gain);
        rule.mute = policy.gain->mute.value_or(false) ? 1U : 0U;
        if (policy.gain->mute.has_value()) {
            ds.user_level.mute = policy.gain->mute;
        }
    }
    rule.position = policy_position(policy.position);
    MradmPolicyDirectBlock block{ds.gain,
                                 ds.user_level.gain_multiplier,
                                 ds.has_position ? 1U : 0U,
                                 {ds.azimuth, ds.elevation, ds.distance},
                                 ds.user_position_override ? 1U : 0U};
    policy_check(mradm_dsp_policy_direct(&rule, &block));
    ds.gain = block.gain;
    ds.user_level.gain_multiplier = block.user_gain_multiplier;
    ds.has_position = block.has_position != 0U;
    ds.azimuth = block.polar[0];
    ds.elevation = block.polar[1];
    ds.distance = block.polar[2];
    ds.user_position_override = block.user_position_override != 0U;
}

void apply_override(SceneObjectBlock& block, const SemanticPolicyOverride& policy, uint32_t sample_rate) {
    if (policy.head_locked.has_value()) {
        block.head_locked = *policy.head_locked;
    }
    if (policy.channel_lock) {
        if (policy.channel_lock->enabled.has_value()) {
            block.channel_lock = *policy.channel_lock->enabled;
            if (!block.channel_lock) {
                block.channel_lock_max_distance.reset();
            }
        }
        if (block.channel_lock && policy.channel_lock->max_distance.has_value()) {
            block.channel_lock_max_distance = policy.channel_lock->max_distance;
        }
    }
    if (policy.interpolation && policy.interpolation->honor_jump_position.has_value() &&
        !*policy.interpolation->honor_jump_position) {
        block.jump_position = false;
    }

    MradmPolicyObject rule{};
    rule.diffuse = policy_section(policy.diffuse);
    if (policy.diffuse) {
        rule.diffuse_scale = policy_value(policy.diffuse->scale);
        rule.diffuse_max = policy_value(policy.diffuse->max);
    }
    rule.extent = policy_section(policy.extent);
    if (policy.extent) {
        rule.extent_scale = policy_value(policy.extent->scale);
        rule.width_scale = policy_value(policy.extent->width_scale);
        rule.height_scale = policy_value(policy.extent->height_scale);
        rule.depth_scale = policy_value(policy.extent->depth_scale);
        rule.extent_max = policy_value(policy.extent->max);
    }
    rule.divergence = policy_section(policy.divergence);
    if (policy.divergence) {
        rule.divergence_scale = policy_value(policy.divergence->scale);
        rule.divergence_range_scale = policy_value(policy.divergence->range_scale);
        rule.divergence_max_range = policy_value(policy.divergence->max_range);
    }
    rule.position = policy_position(policy.position);
    if (policy.interpolation) {
        rule.interpolation_max_ms = policy.interpolation->max_ms.value_or(0U);
    }

    const auto& p = block.position;
    MradmPolicyBlock numeric{};
    numeric.cartesian = p.cartesian ? 1U : 0U;
    numeric.position[0] = p.cartesian ? p.x : p.azimuth;
    numeric.position[1] = p.cartesian ? p.y : p.elevation;
    numeric.position[2] = p.cartesian ? p.z : p.distance;
    numeric.diffuse = block.diffuse;
    numeric.width = block.width;
    numeric.height = block.height;
    numeric.depth = block.depth;
    numeric.divergence = block.divergence;
    numeric.divergence_azimuth_range = block.divergence_azimuth_range;
    numeric.divergence_position_range = block.divergence_position_range;
    numeric.has_interpolation = block.interp_length_samples.has_value() ? 1U : 0U;
    numeric.interpolation_samples = block.interp_length_samples.value_or(0U);
    policy_check(mradm_dsp_policy_object(&rule, sample_rate, &numeric));

    if (p.cartesian && numeric.cartesian == 0U) {
        // Re-aimed Cartesian blocks become polar, as scene_position_to_polar would build them.
        block.position = SceneBlockPosition{};
    }
    if (numeric.cartesian == 0U) {
        block.position.azimuth = numeric.position[0];
        block.position.elevation = numeric.position[1];
        block.position.distance = numeric.position[2];
    }
    block.diffuse = numeric.diffuse;
    block.width = numeric.width;
    block.height = numeric.height;
    block.depth = numeric.depth;
    block.divergence = numeric.divergence;
    block.divergence_azimuth_range = numeric.divergence_azimuth_range;
    block.divergence_position_range = numeric.divergence_position_range;
    if (block.interp_length_samples.has_value()) {
        block.interp_length_samples = numeric.interpolation_samples;
    }
}

[[nodiscard]] std::string rule_label(const SemanticObjectRule& rule) {
    if (!rule.id.empty()) {
        return "id=" + rule.id;
    }
    if (!rule.track_uid.empty()) {
        return "track_uid=" + rule.track_uid;
    }
    if (!rule.name.empty()) {
        return "name=" + rule.name;
    }
    if (!rule.name_glob.empty()) {
        return "name_glob=" + rule.name_glob;
    }
    if (!rule.content.empty()) {
        return "content=" + rule.content;
    }
    if (!rule.programme.empty()) {
        return "programme=" + rule.programme;
    }
    if (!rule.pack_format.empty()) {
        return "pack_format=" + rule.pack_format;
    }
    if (rule.dialogue_id.has_value()) {
        return "dialogue_id=" + std::to_string(*rule.dialogue_id);
    }
    if (rule.importance_min.has_value() || rule.importance_max.has_value()) {
        return fmt::format("importance=[{},{}]",
                           rule.importance_min.has_value() ? std::to_string(*rule.importance_min) : "",
                           rule.importance_max.has_value() ? std::to_string(*rule.importance_max) : "");
    }
    return "all";
}

} // namespace

// NOLINTNEXTLINE(readability-function-size): linear validation keeps field errors local and explicit.
Result<SemanticPolicy> parse_semantic_policy(std::string_view json, std::string source_label) {
    Json doc;
    try {
        doc = Json::parse(json);
    } catch (const Json::exception& e) {
        return tl::unexpected{invalid_policy(source_label, e.what())};
    }
    // Report problems against the source location: a file path (load_semantic_policy_file)
    // or a synthetic label for in-memory callers (the C ABI). Wrapping it in a path keeps
    // the validation body's existing path.string() calls unchanged.
    const std::filesystem::path path = std::move(source_label);
    if (!doc.is_object()) {
        return tl::unexpected{invalid_policy(path.string(), "top-level value must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(doc, {"schema", "global", "objects"}, unknown)) {
        return tl::unexpected{invalid_policy(path.string(), "unknown top-level field '" + unknown + "'")};
    }

    if (!doc.contains("schema")) {
        return tl::unexpected{invalid_policy(path.string(), "missing required field 'schema'")};
    }
    auto schema = read_string(doc, "schema", path.string());
    if (!schema) {
        return tl::unexpected{schema.error()};
    }
    if (*schema != SemanticPolicy::schema_id) {
        return tl::unexpected{invalid_policy(path.string(), fmt::format("unsupported schema '{}'", *schema))};
    }

    SemanticPolicy out;
    if (doc.contains("global")) {
        auto global = parse_override(doc.at("global"), path.string());
        if (!global) {
            return tl::unexpected{global.error()};
        }
        out.global = *global;
    }

    if (doc.contains("objects")) {
        const auto& objects = doc.at("objects");
        if (!objects.is_array()) {
            return tl::unexpected{invalid_policy(path.string(), "'objects' must be an array")};
        }
        for (std::size_t i = 0; i < objects.size(); ++i) {
            const auto& item = objects.at(i);
            if (!item.is_object()) {
                return tl::unexpected{invalid_policy(path.string(), fmt::format("'objects[{}]' must be an object", i))};
            }
            auto parsed = parse_override(item,
                                         path.string(),
                                         {"id",
                                          "name",
                                          "name_glob",
                                          "track_uid",
                                          "all",
                                          "importance_min",
                                          "importance_max",
                                          "dialogue_id",
                                          "content",
                                          "programme",
                                          "pack_format"});
            if (!parsed) {
                return tl::unexpected{parsed.error()};
            }

            SemanticObjectRule rule;
            static_cast<SemanticPolicyOverride&>(rule) = *parsed;
            for (const auto& [field, dst] : {std::pair{std::string_view{"id"}, &rule.id},
                                             std::pair{std::string_view{"name"}, &rule.name},
                                             std::pair{std::string_view{"name_glob"}, &rule.name_glob},
                                             std::pair{std::string_view{"track_uid"}, &rule.track_uid},
                                             std::pair{std::string_view{"content"}, &rule.content},
                                             std::pair{std::string_view{"programme"}, &rule.programme},
                                             std::pair{std::string_view{"pack_format"}, &rule.pack_format}}) {
                if (item.contains(std::string{field})) {
                    auto value = read_string(item, field, path.string());
                    if (!value) {
                        return tl::unexpected{value.error()};
                    }
                    *dst = *value;
                }
            }
            if (item.contains("all")) {
                auto value = read_bool(item, "all", path.string());
                if (!value) {
                    return tl::unexpected{value.error()};
                }
                rule.all = *value;
            }
            for (const auto& [field, dst] : {std::pair{std::string_view{"importance_min"}, &rule.importance_min},
                                             std::pair{std::string_view{"importance_max"}, &rule.importance_max},
                                             std::pair{std::string_view{"dialogue_id"}, &rule.dialogue_id}}) {
                if (item.contains(std::string{field})) {
                    auto value = read_number<int>(item, field, path.string());
                    if (!value) {
                        return tl::unexpected{value.error()};
                    }
                    *dst = *value;
                }
            }
            const bool has_match =
                !rule.id.empty() || !rule.name.empty() || !rule.name_glob.empty() || !rule.track_uid.empty() ||
                !rule.content.empty() || !rule.programme.empty() || !rule.pack_format.empty() || rule.all.has_value() ||
                rule.importance_min.has_value() || rule.importance_max.has_value() || rule.dialogue_id.has_value();
            if (!has_match) {
                return tl::unexpected{
                    invalid_policy(path.string(), fmt::format("'objects[{}]' must define a match field", i))};
            }
            out.objects.push_back(std::move(rule));
        }
    }
    return out;
}

ResolvedSemanticPolicy resolve_semantic_policy(const SemanticPolicy& policy, const SemanticPolicyIdentity& identity) {
    const auto matches = [&](const SemanticObjectRule& rule) {
        if (rule.all.value_or(false)) {
            return true;
        }
        if (!rule.id.empty() && ascii_equal_ignore_case(identity.object_id, rule.id)) {
            return true;
        }
        if (!rule.name.empty() && ascii_equal_ignore_case(identity.object_name, rule.name)) {
            return true;
        }
        if (!rule.name_glob.empty() && glob_match_lower(rule.name_glob, identity.object_name)) {
            return true;
        }
        if (!rule.track_uid.empty() && any_equal_ignore_case(identity.track_uids, rule.track_uid)) {
            return true;
        }
        if ((rule.importance_min || rule.importance_max) && identity.importance.has_value()) {
            const int value = *identity.importance;
            if (value >= rule.importance_min.value_or(std::numeric_limits<int>::min()) &&
                value <= rule.importance_max.value_or(std::numeric_limits<int>::max())) {
                return true;
            }
        }
        if (rule.dialogue_id && identity.dialogue_id.has_value() && *identity.dialogue_id == *rule.dialogue_id) {
            return true;
        }
        if (!rule.content.empty() && (any_equal_ignore_case(identity.content_ids, rule.content) ||
                                      any_equal_ignore_case(identity.content_names, rule.content))) {
            return true;
        }
        if (!rule.programme.empty() && (any_equal_ignore_case(identity.programme_ids, rule.programme) ||
                                        any_equal_ignore_case(identity.programme_names, rule.programme))) {
            return true;
        }
        return false;
    };

    ResolvedSemanticPolicy out;
    if (policy.global) {
        merge_override(out.object, *policy.global);
        if (policy.global->direct_speakers) {
            out.direct_speakers.push_back(*policy.global->direct_speakers);
        }
    }
    for (std::size_t index = 0U; index < policy.objects.size(); ++index) {
        const auto& rule = policy.objects[index];
        if (!matches(rule)) {
            continue;
        }
        out.matched_rule_indices.push_back(index);
        merge_override(out.object, rule);
        if (rule.direct_speakers) {
            out.direct_speakers.push_back(*rule.direct_speakers);
        }
    }
    return out;
}

void apply_resolved_semantic_object(SceneObject& object, const SemanticPolicyOverride& policy) {
    apply_object_override(object, policy);
}

void apply_resolved_semantic_object_block(SceneObjectBlock& block,
                                          const SemanticPolicyOverride& policy,
                                          uint32_t sample_rate) {
    apply_override(block, policy, sample_rate);
}

void apply_resolved_semantic_direct_speaker(SceneDirectSpeakersBlock& block,
                                            std::span<const DirectSpeakersPolicy> policies,
                                            const SemanticPolicyOverride& object_policy) {
    if (object_policy.head_locked.has_value()) {
        block.head_locked = *object_policy.head_locked;
    }
    for (const auto& policy : policies) {
        apply_ds_override(block, policy);
    }
}

Result<void> apply_semantic_policy(AdmScene& scene,
                                   const SemanticPolicy& policy,
                                   uint32_t sample_rate,
                                   std::vector<std::string>* warnings) {
    const auto membership = build_membership(scene);
    const ObjectMembership empty_membership;
    const auto membership_for = [&](const SceneObject& object) -> const ObjectMembership& {
        const auto it = membership.find(object.id);
        return it != membership.end() ? it->second : empty_membership;
    };

    for (const auto& rule : policy.objects) {
        const bool matched_object = std::ranges::any_of(scene.objects, [&](const SceneObject& object) {
            return rule_matches(object, membership_for(object), rule);
        });
        const bool matched_hoa = std::ranges::any_of(
            scene.hoa_tracks, [&](const SceneHOATracks& pack) { return hoa_rule_matches(pack, rule); });
        if (!matched_object && !matched_hoa && warnings != nullptr) {
            warnings->push_back("semantic policy object rule did not match: " + rule_label(rule));
        }
    }

    for (auto& object : scene.objects) {
        const auto& mem = membership_for(object);
        SemanticPolicyIdentity identity;
        identity.object_id = object.id;
        identity.object_name = object.name;
        identity.importance = object.importance;
        if (object.dialogue_id.has_value()) {
            identity.dialogue_id = static_cast<int>(*object.dialogue_id);
        }
        std::ranges::transform(object.tracks, std::back_inserter(identity.track_uids), &SceneTrackRef::track_uid);
        identity.content_ids = mem.content_ids;
        identity.content_names = mem.content_names;
        identity.programme_ids = mem.programme_ids;
        identity.programme_names = mem.programme_names;
        const auto resolved = resolve_semantic_policy(policy, identity);
        apply_resolved_semantic_object(object, resolved.object);
        // DS policies are applied per-rule (each with its own filter), in order,
        // rather than merged — so independent rules (e.g. one targeting M+000 and
        // one targeting LFE) don't cross-contaminate each other's filters.
        for (auto& track : object.tracks) {
            for (auto& block : track.blocks) {
                apply_resolved_semantic_object_block(block, resolved.object, sample_rate);
            }
            for (auto& ds : track.ds_blocks) {
                apply_resolved_semantic_direct_speaker(ds, resolved.direct_speakers, resolved.object);
            }
        }
    }

    // HOA packs (separate from scene.objects): gain/mute via id / pack_format / all.
    for (auto& pack : scene.hoa_tracks) {
        if (const auto gain = effective_hoa_gain(pack, policy)) {
            apply_hoa_gain(pack, *gain);
        }
        if (const auto head_locked = effective_hoa_head_locked(pack, policy)) {
            for (auto& channel : pack.channels) {
                for (auto& block : channel.blocks) {
                    block.head_locked = *head_locked;
                }
            }
        }
    }
    return {};
}

} // namespace mradm
