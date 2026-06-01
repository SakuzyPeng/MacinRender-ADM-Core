#include "adm/semantic_policy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

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

[[nodiscard]] Error invalid_policy(const std::string& path, const std::string& message) {
    return {ErrorCode::invalid_argument, "invalid semantic policy: " + message, path};
}

[[nodiscard]] Error io_policy(const std::filesystem::path& path, const std::string& message) {
    return {ErrorCode::io_error, "semantic policy I/O error: " + message, path.string()};
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
    if (has_unknown_keys(obj, {"speaker_label", "lfe", "gain", "position"}, unknown)) {
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
                                          k_direct_speakers_key};
    allowed.insert(allowed.end(), extra_keys.begin(), extra_keys.end());
    for (const auto& [key, value] : obj.items()) {
        (void) value;
        if (std::ranges::find(allowed, std::string_view{key}) == allowed.end()) {
            return tl::unexpected{invalid_policy(path, "unknown policy field '" + key + "'")};
        }
    }

    SemanticPolicyOverride out;
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
        out, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
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

[[nodiscard]] std::vector<std::string>
matched_rule_names(const SceneObject& object, const ObjectMembership& membership, const SemanticPolicy& policy) {
    std::vector<std::string> out;
    if (policy.global.has_value()) {
        out.emplace_back("global");
    }
    for (std::size_t i = 0; i < policy.objects.size(); ++i) {
        if (rule_matches(object, membership, policy.objects.at(i))) {
            out.push_back(fmt::format("objects[{}]", i));
        }
    }
    return out;
}

[[nodiscard]] SemanticPolicyOverride
effective_override(const SceneObject& object, const ObjectMembership& membership, const SemanticPolicy& policy) {
    SemanticPolicyOverride out;
    if (policy.global) {
        merge_override(out, *policy.global);
    }
    for (const auto& rule : policy.objects) {
        if (rule_matches(object, membership, rule)) {
            merge_override(out, rule);
        }
    }
    return out;
}

// Ordered list of DirectSpeakers policies applicable to an object (global first,
// then matching object rules in order). Each carries its own per-block filter and
// is applied independently — they are NOT merged (see merge note above).
[[nodiscard]] std::vector<const DirectSpeakersPolicy*>
collect_ds_policies(const SceneObject& object, const ObjectMembership& membership, const SemanticPolicy& policy) {
    std::vector<const DirectSpeakersPolicy*> out;
    if (policy.global && policy.global->direct_speakers) {
        out.push_back(&*policy.global->direct_speakers);
    }
    for (const auto& rule : policy.objects) {
        if (rule.direct_speakers && rule_matches(object, membership, rule)) {
            out.push_back(&*rule.direct_speakers);
        }
    }
    return out;
}

// Object-level (per AudioObject) application: gain + mute. Called once per object.
void apply_object_override(SceneObject& object, const SemanticPolicyOverride& policy) {
    if (policy.gain) {
        if (policy.gain->scale.has_value()) {
            object.gain *= *policy.gain->scale;
        }
        if (policy.gain->gain_db.has_value()) {
            object.gain *= std::pow(10.0F, *policy.gain->gain_db / 20.0F);
        }
        if (policy.gain->mute.has_value()) {
            object.mute = *policy.gain->mute;
        }
    }
}

// HOA packs live outside scene.objects and are matched by object_id / pack_format
// / all. Only gain/mute apply (the soundfield's per-channel gains are not touched).
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

[[nodiscard]] std::vector<std::string> matched_hoa_rule_names(const SceneHOATracks& pack,
                                                              const SemanticPolicy& policy) {
    std::vector<std::string> out;
    if (policy.global && policy.global->gain) {
        out.emplace_back("global");
    }
    for (std::size_t i = 0; i < policy.objects.size(); ++i) {
        if (policy.objects.at(i).gain && hoa_rule_matches(pack, policy.objects.at(i))) {
            out.push_back(fmt::format("objects[{}]", i));
        }
    }
    return out;
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
    if (gain.scale.has_value()) {
        pack.gain *= *gain.scale;
    }
    if (gain.gain_db.has_value()) {
        pack.gain *= std::pow(10.0F, *gain.gain_db / 20.0F);
    }
}

// True when the position policy would actually change a position: any absolute
// or lock field, or a non-absent, non-zero offset component. A neutral template's
// zero-offset position section is therefore inert.
[[nodiscard]] bool position_changes(const PositionPolicy& pos) {
    const auto nonzero = [](const std::optional<float>& v) { return v.has_value() && (*v > 0.0F || *v < 0.0F); };
    const bool offset_changes =
        pos.offset && (nonzero(pos.offset->azimuth) || nonzero(pos.offset->elevation) || nonzero(pos.offset->distance));
    return pos.azimuth.has_value() || pos.elevation.has_value() || pos.distance.has_value() ||
           pos.lock_azimuth.has_value() || pos.lock_elevation.has_value() || offset_changes;
}

// Shared polar position math: absolute overwrite, then offset add, then lock
// (wins), then wrap azimuth / clamp elevation / non-negative distance. Mutates
// az/el/dist in place. Caller guards with position_changes() before invoking.
void compute_polar_position(float& azimuth, float& elevation, float& distance, const PositionPolicy& pos) {
    if (pos.azimuth.has_value()) {
        azimuth = *pos.azimuth;
    }
    if (pos.elevation.has_value()) {
        elevation = *pos.elevation;
    }
    if (pos.distance.has_value()) {
        distance = *pos.distance;
    }
    if (pos.offset) {
        azimuth += pos.offset->azimuth.value_or(0.0F);
        elevation += pos.offset->elevation.value_or(0.0F);
        distance += pos.offset->distance.value_or(0.0F);
    }
    if (pos.lock_azimuth.has_value()) {
        azimuth = *pos.lock_azimuth;
    }
    if (pos.lock_elevation.has_value()) {
        elevation = *pos.lock_elevation;
    }
    azimuth = wrap_azimuth(azimuth);
    elevation = std::clamp(elevation, -90.0F, 90.0F);
    distance = std::max(0.0F, distance);
}

// Objects block position override. Cartesian positions are converted to polar
// first; output is always polar. No-op when the policy makes no change.
void apply_position(SceneObjectBlock& block, const PositionPolicy& pos) {
    if (!position_changes(pos)) {
        return;
    }
    SceneBlockPosition polar = scene_position_to_polar(block.position);
    polar.cartesian = false;
    compute_polar_position(polar.azimuth, polar.elevation, polar.distance, pos);
    block.position = polar;
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
    if (policy.gain) {
        if (policy.gain->mute.value_or(false)) {
            ds.gain = 0.0F;
        } else {
            if (policy.gain->scale.has_value()) {
                ds.gain *= *policy.gain->scale;
            }
            if (policy.gain->gain_db.has_value()) {
                ds.gain *= std::pow(10.0F, *policy.gain->gain_db / 20.0F);
            }
        }
    }
    if (policy.position && position_changes(*policy.position)) {
        compute_polar_position(ds.azimuth, ds.elevation, ds.distance, *policy.position);
        ds.has_position = true;
    }
}

void apply_override(SceneObjectBlock& block, const SemanticPolicyOverride& policy, uint32_t sample_rate) {
    if (policy.diffuse) {
        if (policy.diffuse->enabled.has_value() && !*policy.diffuse->enabled) {
            block.diffuse = 0.0F;
        } else {
            block.diffuse *= policy.diffuse->scale.value_or(1.0F);
            block.diffuse = std::clamp(block.diffuse, 0.0F, policy.diffuse->max.value_or(1.0F));
        }
    }

    if (policy.extent) {
        if (policy.extent->enabled.has_value() && !*policy.extent->enabled) {
            block.width = 0.0F;
            block.height = 0.0F;
            block.depth = 0.0F;
        } else {
            const float scale = policy.extent->scale.value_or(1.0F);
            block.width *= scale * policy.extent->width_scale.value_or(1.0F);
            block.height *= scale * policy.extent->height_scale.value_or(1.0F);
            block.depth *= scale * policy.extent->depth_scale.value_or(1.0F);
            const float max_value = policy.extent->max.value_or(1.0F);
            block.width = std::clamp(block.width, 0.0F, max_value);
            block.height = std::clamp(block.height, 0.0F, max_value);
            block.depth = std::clamp(block.depth, 0.0F, max_value);
        }
    }

    if (policy.divergence) {
        if (policy.divergence->enabled.has_value() && !*policy.divergence->enabled) {
            block.divergence = 0.0F;
        } else {
            block.divergence = std::clamp(block.divergence * policy.divergence->scale.value_or(1.0F), 0.0F, 1.0F);
            const float range_scale = policy.divergence->range_scale.value_or(1.0F);
            const float max_range = policy.divergence->max_range.value_or(120.0F);
            block.divergence_azimuth_range = std::clamp(block.divergence_azimuth_range * range_scale, 0.0F, max_range);
            block.divergence_position_range = std::max(0.0F, block.divergence_position_range * range_scale);
        }
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

    if (policy.position) {
        apply_position(block, *policy.position);
    }

    if (policy.interpolation) {
        if (policy.interpolation->honor_jump_position.has_value() && !*policy.interpolation->honor_jump_position) {
            block.jump_position = false;
        }
        const uint32_t max_ms = policy.interpolation->max_ms.value_or(0U);
        if (max_ms > 0U && block.interp_length_samples.has_value()) {
            const uint64_t max_samples = (static_cast<uint64_t>(sample_rate) * max_ms) / 1000U;
            block.interp_length_samples = std::min(*block.interp_length_samples, max_samples);
        }
    }
}

[[nodiscard]] Json block_json(const SceneObjectBlock& block) {
    Json out = Json::object();
    out["diffuse"] = block.diffuse;
    out["width"] = block.width;
    out["height"] = block.height;
    out["depth"] = block.depth;
    out["divergence"] = block.divergence;
    out["divergence_azimuth_range"] = block.divergence_azimuth_range;
    out["divergence_position_range"] = block.divergence_position_range;
    out["channel_lock"] = block.channel_lock;
    if (block.channel_lock_max_distance) {
        out["channel_lock_max_distance"] = *block.channel_lock_max_distance;
    } else {
        out["channel_lock_max_distance"] = nullptr;
    }
    out["jump_position"] = block.jump_position;
    if (block.interp_length_samples) {
        out["interp_length_samples"] = *block.interp_length_samples;
    } else {
        out["interp_length_samples"] = nullptr;
    }
    // Position (always reported as polar for stable diffing).
    const SceneBlockPosition polar = scene_position_to_polar(block.position);
    out["azimuth"] = polar.azimuth;
    out["elevation"] = polar.elevation;
    out["distance"] = polar.distance;
    return out;
}

[[nodiscard]] Json ds_block_json(const SceneDirectSpeakersBlock& ds) {
    Json out = Json::object();
    out["speaker_labels"] = ds.speaker_labels;
    out["gain"] = ds.gain;
    out["has_position"] = ds.has_position;
    out["azimuth"] = ds.azimuth;
    out["elevation"] = ds.elevation;
    out["distance"] = ds.distance;
    if (ds.low_pass_hz) {
        out["low_pass_hz"] = *ds.low_pass_hz;
    } else {
        out["low_pass_hz"] = nullptr;
    }
    return out;
}

[[nodiscard]] Json capability_json(const CapabilityReport& caps) {
    Json out = Json::object();
    out["supports_channel_lock"] = caps.supports_channel_lock;
    out["supports_object_divergence"] = caps.supports_object_divergence;
    out["supports_diffuse"] = caps.supports_diffuse;
    return out;
}

[[nodiscard]] Json neutral_diffuse_policy() {
    return Json{{"enabled", true}, {"scale", 1.0F}, {"max", 1.0F}};
}

[[nodiscard]] Json neutral_extent_policy() {
    return Json{{"enabled", true},
                {"scale", 1.0F},
                {"width_scale", 1.0F},
                {"height_scale", 1.0F},
                {"depth_scale", 1.0F},
                {"max", 1.0F}};
}

[[nodiscard]] Json neutral_divergence_policy() {
    return Json{{"enabled", true}, {"scale", 1.0F}, {"range_scale", 1.0F}, {"max_range", 120.0F}};
}

[[nodiscard]] Json neutral_interpolation_policy() {
    return Json{{"honor_jump_position", true}, {"max_ms", 0U}};
}

// Neutral gain = identity. mute is intentionally omitted: its mere presence
// would force object.mute, un-muting originally muted objects.
[[nodiscard]] Json neutral_gain_policy() {
    return Json{{"scale", 1.0F}, {"gain_db", 0.0F}};
}

// Neutral position = zero offset only (a no-op under apply_position's guard).
// Absolute / lock keys are intentionally omitted: their mere presence would
// overwrite the block position.
[[nodiscard]] Json neutral_position_policy() {
    return Json{{"offset", {{"azimuth", 0.0F}, {"elevation", 0.0F}, {"distance", 0.0F}}}};
}

// Neutral DirectSpeakers = identity. No speaker_label/lfe filter (applies to all
// DS blocks), no mute; gain x1 and zero-offset position are both no-ops.
[[nodiscard]] Json neutral_direct_speakers_policy() {
    return Json{{"gain", neutral_gain_policy()}, {"position", neutral_position_policy()}};
}

// The generated template, applied unmodified, must be an identity (no scene
// change). channel_lock is intentionally omitted: it has no neutral value —
// `enabled` forces the lock on/off either way — so users add it explicitly.
[[nodiscard]] Json neutral_override_template() {
    Json out = Json::object();
    out["diffuse"] = neutral_diffuse_policy();
    out["extent"] = neutral_extent_policy();
    out["divergence"] = neutral_divergence_policy();
    out["interpolation"] = neutral_interpolation_policy();
    out["gain"] = neutral_gain_policy();
    out["position"] = neutral_position_policy();
    out["direct_speakers"] = neutral_direct_speakers_policy();
    return out;
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

// HOA packs (separate from objects): report gain/mute original -> effective.
[[nodiscard]] Json
hoa_tracks_report_json(const AdmScene& original, const AdmScene& effective, const SemanticPolicy* policy) {
    Json out = Json::array();
    const std::size_t hoa_count = std::min(original.hoa_tracks.size(), effective.hoa_tracks.size());
    for (std::size_t hi = 0; hi < hoa_count; ++hi) {
        const auto& orig_pack = original.hoa_tracks.at(hi);
        const auto& eff_pack = effective.hoa_tracks.at(hi);
        Json pack = Json::object();
        pack["object_id"] = eff_pack.object_id;
        pack["pack_format_id"] = eff_pack.pack_format_id;
        pack["matched_rules"] =
            policy != nullptr ? matched_hoa_rule_names(orig_pack, *policy) : std::vector<std::string>{};
        pack["original_gain"] = orig_pack.gain;
        pack["effective_gain"] = eff_pack.gain;
        pack["original_mute"] = orig_pack.mute;
        pack["effective_mute"] = eff_pack.mute;
        out.push_back(std::move(pack));
    }
    return out;
}

} // namespace

// NOLINTNEXTLINE(readability-function-size): linear validation keeps field errors local and explicit.
Result<SemanticPolicy> parse_semantic_policy(std::string_view json, std::string source_label) {
    Json doc;
    try {
        doc = Json::parse(json);
    } catch (const Json::parse_error& e) {
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

Result<SemanticPolicy> load_semantic_policy_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return tl::unexpected{io_policy(path, "cannot open file")};
    }
    std::stringstream buf;
    buf << in.rdbuf();
    return parse_semantic_policy(buf.str(), path.string());
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
        const auto policy_for_object = effective_override(object, mem, policy);
        apply_object_override(object, policy_for_object);
        // DS policies are applied per-rule (each with its own filter), in order,
        // rather than merged — so independent rules (e.g. one targeting M+000 and
        // one targeting LFE) don't cross-contaminate each other's filters.
        const auto ds_policies = collect_ds_policies(object, mem, policy);
        for (auto& track : object.tracks) {
            for (auto& block : track.blocks) {
                apply_override(block, policy_for_object, sample_rate);
            }
            for (auto& ds : track.ds_blocks) {
                for (const auto* ds_policy : ds_policies) {
                    apply_ds_override(ds, *ds_policy);
                }
            }
        }
    }

    // HOA packs (separate from scene.objects): gain/mute via id / pack_format / all.
    for (auto& pack : scene.hoa_tracks) {
        if (const auto gain = effective_hoa_gain(pack, policy)) {
            apply_hoa_gain(pack, *gain);
        }
    }
    return {};
}

std::string build_semantic_report(const AdmScene& original,
                                  const AdmScene& effective,
                                  const SemanticPolicy* policy,
                                  const SemanticPolicyReportOptions& options,
                                  const std::vector<std::string>& warnings) {
    Json doc = Json::object();
    doc["schema"] = "mradm.semantic-report.v1";
    doc["renderer"] = options.renderer;
    doc["policy"] = options.policy_path.empty() ? nullptr : Json{options.policy_path};
    doc["capabilities"] = capability_json(options.capabilities);
    doc["warnings"] = warnings;
    doc["objects"] = Json::array();

    const auto membership = build_membership(original);
    const ObjectMembership empty_membership;

    const std::size_t object_count = std::min(original.objects.size(), effective.objects.size());
    for (std::size_t oi = 0; oi < object_count; ++oi) {
        const auto& orig_obj = original.objects.at(oi);
        const auto& eff_obj = effective.objects.at(oi);
        const auto mem_it = membership.find(orig_obj.id);
        const ObjectMembership& mem = mem_it != membership.end() ? mem_it->second : empty_membership;
        Json obj = Json::object();
        obj["id"] = eff_obj.id;
        obj["name"] = eff_obj.name;
        obj["matched_rules"] =
            policy != nullptr ? matched_rule_names(orig_obj, mem, *policy) : std::vector<std::string>{};
        // Object-level gain/mute (changed by gain policy; not part of per-block detail).
        obj["original_gain"] = orig_obj.gain;
        obj["effective_gain"] = eff_obj.gain;
        obj["original_mute"] = orig_obj.mute;
        obj["effective_mute"] = eff_obj.mute;
        obj["blocks"] = Json::array();

        const std::size_t track_count = std::min(orig_obj.tracks.size(), eff_obj.tracks.size());
        for (std::size_t ti = 0; ti < track_count; ++ti) {
            const auto& orig_track = orig_obj.tracks.at(ti);
            const auto& eff_track = eff_obj.tracks.at(ti);
            const std::size_t block_count = std::min(orig_track.blocks.size(), eff_track.blocks.size());
            for (std::size_t bi = 0; bi < block_count; ++bi) {
                Json block = Json::object();
                block["kind"] = "objects";
                block["track_uid"] = eff_track.track_uid;
                block["block_index"] = bi;
                block["original"] = block_json(orig_track.blocks.at(bi));
                block["effective"] = block_json(eff_track.blocks.at(bi));
                obj["blocks"].push_back(std::move(block));
            }
            const std::size_t ds_count = std::min(orig_track.ds_blocks.size(), eff_track.ds_blocks.size());
            for (std::size_t bi = 0; bi < ds_count; ++bi) {
                Json block = Json::object();
                block["kind"] = "directspeakers";
                block["track_uid"] = eff_track.track_uid;
                block["block_index"] = bi;
                block["original"] = ds_block_json(orig_track.ds_blocks.at(bi));
                block["effective"] = ds_block_json(eff_track.ds_blocks.at(bi));
                obj["blocks"].push_back(std::move(block));
            }
        }
        doc["objects"].push_back(std::move(obj));
    }

    doc["hoa_tracks"] = hoa_tracks_report_json(original, effective, policy);

    return doc.dump(2);
}

Result<void> write_semantic_report_file(const std::filesystem::path& path,
                                        const AdmScene& original,
                                        const AdmScene& effective,
                                        const SemanticPolicy* policy,
                                        const SemanticPolicyReportOptions& options,
                                        const std::vector<std::string>& warnings) {
    std::ofstream out(path);
    if (!out) {
        return tl::unexpected{io_policy(path, "cannot open report for writing")};
    }
    out << build_semantic_report(original, effective, policy, options, warnings) << '\n';
    if (!out) {
        return tl::unexpected{io_policy(path, "failed to write report")};
    }
    return {};
}

std::string build_semantic_policy_template(const AdmScene& scene) {
    Json doc = Json::object();
    doc["schema"] = SemanticPolicy::schema_id;
    doc["global"] = neutral_override_template();
    doc["objects"] = Json::array();

    for (const auto& object : scene.objects) {
        Json rule = neutral_override_template();
        rule["id"] = object.id;
        rule["name"] = object.name;
        if (!object.tracks.empty()) {
            rule["track_uid"] = object.tracks.front().track_uid;
        }
        doc["objects"].push_back(std::move(rule));
    }
    // HOA packs: only gain/mute apply, so emit a lean neutral rule (no position /
    // diffuse / etc. that are meaningless for a soundfield).
    for (const auto& pack : scene.hoa_tracks) {
        Json rule = Json::object();
        rule["id"] = pack.object_id;
        rule["pack_format"] = pack.pack_format_id;
        rule["gain"] = neutral_gain_policy();
        doc["objects"].push_back(std::move(rule));
    }
    return doc.dump(2);
}

Result<void> write_semantic_policy_template_file(const std::filesystem::path& path, const AdmScene& scene) {
    std::ofstream out(path);
    if (!out) {
        return tl::unexpected{io_policy(path, "cannot open policy template for writing")};
    }
    out << build_semantic_policy_template(scene) << '\n';
    if (!out) {
        return tl::unexpected{io_policy(path, "failed to write policy template")};
    }
    return {};
}

} // namespace mradm
