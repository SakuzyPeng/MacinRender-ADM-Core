#include "adm/direct_speakers_matrix.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace mradm {
namespace {

using Json = nlohmann::json;

[[nodiscard]] Error invalid_matrix(std::string_view source, std::string message) {
    return {ErrorCode::invalid_argument, "invalid DirectSpeakers matrix: " + std::move(message), std::string{source}};
}

[[nodiscard]] Error io_matrix(const std::filesystem::path& path, std::string message) {
    return {ErrorCode::io_error, "DirectSpeakers matrix I/O error: " + std::move(message), path.string()};
}

[[nodiscard]] bool
has_unknown_keys(const Json& object, std::initializer_list<std::string_view> allowed, std::string& unknown) {
    if (!object.is_object()) {
        return false;
    }
    for (const auto& [key, value] : object.items()) {
        (void) value;
        if (!std::ranges::any_of(allowed, [&](std::string_view candidate) { return candidate == key; })) {
            unknown = key;
            return true;
        }
    }
    return false;
}

[[nodiscard]] Result<std::string>
read_required_string(const Json& object, std::string_view key, std::string_view path, std::string_view source) {
    const auto it = object.find(std::string{key});
    if (it == object.end()) {
        return tl::unexpected{invalid_matrix(source, fmt::format("{} is missing required field '{}'", path, key))};
    }
    if (!it->is_string()) {
        return tl::unexpected{invalid_matrix(source, fmt::format("{}.{} must be a string", path, key))};
    }
    auto value = it->get<std::string>();
    if (value.empty()) {
        return tl::unexpected{invalid_matrix(source, fmt::format("{}.{} must not be empty", path, key))};
    }
    return value;
}

[[nodiscard]] Result<DirectSpeakersMatrixTarget>
parse_target(const Json& object, std::size_t route_index, std::size_t target_index, std::string_view source) {
    const auto path = fmt::format("routes[{}].targets[{}]", route_index, target_index);
    if (!object.is_object()) {
        return tl::unexpected{invalid_matrix(source, path + " must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(object, {"label", "weight"}, unknown)) {
        return tl::unexpected{invalid_matrix(source, fmt::format("{} has unknown field '{}'", path, unknown))};
    }

    auto label = read_required_string(object, "label", path, source);
    if (!label) {
        return tl::unexpected{label.error()};
    }
    if (!object.contains("weight")) {
        return tl::unexpected{invalid_matrix(source, path + " is missing required field 'weight'")};
    }
    const auto& weight_json = object.at("weight");
    if (!weight_json.is_number()) {
        return tl::unexpected{invalid_matrix(source, path + ".weight must be numeric")};
    }
    float weight = 0.0F;
    try {
        weight = weight_json.get<float>();
    } catch (const Json::exception& error) {
        return tl::unexpected{invalid_matrix(source, path + ".weight is invalid: " + error.what())};
    }
    if (!std::isfinite(weight) || weight <= 0.0F) {
        return tl::unexpected{invalid_matrix(source, path + ".weight must be finite and > 0")};
    }
    return DirectSpeakersMatrixTarget{std::move(*label), weight, 0.0F};
}

[[nodiscard]] Result<DirectSpeakersMatrixRoute>
parse_route(const Json& object, std::size_t route_index, std::string_view source) {
    const auto path = fmt::format("routes[{}]", route_index);
    if (!object.is_object()) {
        return tl::unexpected{invalid_matrix(source, path + " must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(object, {"source_label", "targets", "mute"}, unknown)) {
        return tl::unexpected{invalid_matrix(source, fmt::format("{} has unknown field '{}'", path, unknown))};
    }

    auto source_label = read_required_string(object, "source_label", path, source);
    if (!source_label) {
        return tl::unexpected{source_label.error()};
    }

    const bool has_targets = object.contains("targets");
    const bool has_mute = object.contains("mute");
    if (has_targets == has_mute) {
        return tl::unexpected{invalid_matrix(source, path + " must define exactly one of 'targets' or 'mute'")};
    }

    DirectSpeakersMatrixRoute route;
    route.source_label = std::move(*source_label);
    if (has_mute) {
        if (!object.at("mute").is_boolean() || !object.at("mute").get<bool>()) {
            return tl::unexpected{invalid_matrix(source, path + ".mute must be true")};
        }
        route.mute = true;
        return route;
    }

    const auto& targets = object.at("targets");
    if (!targets.is_array() || targets.empty()) {
        return tl::unexpected{invalid_matrix(source, path + ".targets must be a non-empty array")};
    }
    route.targets.reserve(targets.size());
    double weight_sum = 0.0;
    for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
        auto target = parse_target(targets.at(target_index), route_index, target_index, source);
        if (!target) {
            return tl::unexpected{target.error()};
        }
        weight_sum += static_cast<double>(target->weight);
        if (!std::isfinite(weight_sum)) {
            return tl::unexpected{invalid_matrix(source, path + ".targets weight sum must be finite")};
        }
        route.targets.push_back(std::move(*target));
    }
    for (auto& target : route.targets) {
        target.gain = static_cast<float>(std::sqrt(static_cast<double>(target.weight) / weight_sum));
    }
    return route;
}

} // namespace

Result<DirectSpeakersMatrix> parse_direct_speakers_matrix(std::string_view json, std::string_view source_label) {
    Json document;
    try {
        document = Json::parse(json);
    } catch (const Json::exception& error) {
        return tl::unexpected{invalid_matrix(source_label, error.what())};
    }
    if (!document.is_object()) {
        return tl::unexpected{invalid_matrix(source_label, "top-level value must be an object")};
    }
    std::string unknown;
    if (has_unknown_keys(document, {"schema", "output_layout", "routes"}, unknown)) {
        return tl::unexpected{invalid_matrix(source_label, "unknown top-level field '" + unknown + "'")};
    }

    auto schema = read_required_string(document, "schema", "top-level value", source_label);
    if (!schema) {
        return tl::unexpected{schema.error()};
    }
    if (*schema != DirectSpeakersMatrix::schema_id) {
        return tl::unexpected{invalid_matrix(source_label, fmt::format("unsupported schema '{}'", *schema))};
    }
    auto output_layout = read_required_string(document, "output_layout", "top-level value", source_label);
    if (!output_layout) {
        return tl::unexpected{output_layout.error()};
    }
    if (!document.contains("routes")) {
        return tl::unexpected{invalid_matrix(source_label, "top-level value is missing required field 'routes'")};
    }
    const auto& routes = document.at("routes");
    if (!routes.is_array() || routes.empty()) {
        return tl::unexpected{invalid_matrix(source_label, "top-level routes must be a non-empty array")};
    }

    DirectSpeakersMatrix matrix;
    matrix.output_layout = std::move(*output_layout);
    matrix.routes.reserve(routes.size());
    for (std::size_t route_index = 0; route_index < routes.size(); ++route_index) {
        auto route = parse_route(routes.at(route_index), route_index, source_label);
        if (!route) {
            return tl::unexpected{route.error()};
        }
        matrix.routes.push_back(std::move(*route));
    }
    return matrix;
}

Result<DirectSpeakersMatrix> load_direct_speakers_matrix_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return tl::unexpected{io_matrix(path, "cannot open file")};
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return tl::unexpected{io_matrix(path, "failed while reading file")};
    }
    return parse_direct_speakers_matrix(buffer.str(), path.string());
}

} // namespace mradm
