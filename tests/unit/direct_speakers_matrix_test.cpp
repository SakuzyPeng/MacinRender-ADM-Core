#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/direct_speakers_matrix.h"
#include "adm/options.h"
#include "adm/scene.h"

#include "render_common.h"

namespace {

constexpr std::string_view k_weighted_matrix = R"json({
  "schema": "mradm.direct-speakers-matrix.v1",
  "output_layout": "5.1",
  "routes": [
    {
      "source_label": "M+000",
      "targets": [
        {"label": "M+030", "weight": 1},
        {"label": "M-030", "weight": 3}
      ]
    },
    {
      "source_label": "M-030",
      "targets": [{"label": "M-030", "weight": 9}]
    },
    {"source_label": "U+000", "mute": true}
  ]
})json";

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

template <typename T>
bool check_error(const mradm::Result<T>& result, std::string_view needle, std::string_view message) {
    const bool matches = !result && (needle.empty() || result.error().message.find(needle) != std::string::npos);
    if (!matches) {
        std::cerr << "FAIL: " << message;
        if (result) {
            std::cerr << " (unexpected success)";
        } else {
            std::cerr << " (error: " << result.error().message << ')';
        }
        std::cerr << '\n';
    }
    return matches;
}

bool almost_equal(float lhs, float rhs, float tolerance = 1.0e-6F) {
    return std::abs(lhs - rhs) <= tolerance;
}

bool verify_parser_and_coefficients() {
    auto parsed = mradm::parse_direct_speakers_matrix(k_weighted_matrix);
    bool ok = check(parsed.has_value(), "valid matrix parses");
    if (!parsed) {
        return false;
    }
    ok &= check(parsed->output_layout == "5.1", "output layout is retained for later normalisation");
    ok &= check(parsed->routes.size() == 3U, "all matrix rows are parsed");
    ok &= check(parsed->routes[0].targets.size() == 2U, "one-to-many route is retained");
    ok &= check(almost_equal(parsed->routes[0].targets[0].gain, 0.5F), "1:3 route first coefficient is sqrt(1/4)");
    ok &= check(almost_equal(parsed->routes[0].targets[1].gain, std::sqrt(0.75F)),
                "1:3 route second coefficient is sqrt(3/4)");
    ok &= check(almost_equal(parsed->routes[1].targets[0].gain, 1.0F), "one-to-one route coefficient is unity");
    ok &= check(parsed->routes[2].mute && parsed->routes[2].targets.empty(), "explicit mute route has no targets");
    return ok;
}

bool verify_strict_schema_errors() {
    struct InvalidCase {
        std::string_view json;
        std::string_view error;
        std::string_view description;
    };
    constexpr std::array<InvalidCase, 11> cases{{
        {R"json([])json", "top-level value", "non-object document is rejected"},
        {R"json({"schema":"wrong","output_layout":"5.1","routes":[{"source_label":"C","mute":true}]})json",
         "unsupported schema",
         "unknown schema is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","mute":true}],"extra":1})json",
         "unknown top-level field",
         "unknown top-level field is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","mute":true,"extra":1}]})json",
         "unknown field",
         "unknown route field is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[{"label":"L","weight":1,"extra":1}]}]})json",
         "unknown field",
         "unknown target field is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[],"mute":true}]})json",
         "exactly one",
         "targets and mute together are rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","mute":false}]})json",
         "must be true",
         "mute false is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[]}]})json",
         "non-empty array",
         "empty target list is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[{"label":"L","weight":0}]}]})json",
         "finite and > 0",
         "zero weight is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[{"label":"L","weight":-1}]}]})json",
         "finite and > 0",
         "negative weight is rejected"},
        {R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[{"label":"L","weight":"1"}]}]})json",
         "must be numeric",
         "non-numeric weight is rejected"},
    }};

    bool ok = true;
    for (const auto& invalid : cases) {
        const auto parsed = mradm::parse_direct_speakers_matrix(invalid.json);
        ok &= check_error(parsed, invalid.error, invalid.description);
    }
    const auto overflow = mradm::parse_direct_speakers_matrix(
        R"json({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[{"source_label":"C","targets":[{"label":"L","weight":1e999}]}]})json");
    ok &= check_error(overflow, {}, "non-finite/overflowing weight is rejected");
    return ok;
}

bool verify_target_and_source_resolution() {
    using mradm::render_common::DirectSpeakerRoutingTarget;
    constexpr std::array<DirectSpeakerRoutingTarget, 4> targets{{
        {"M+030", 30.0F, 0.0F, false},
        {"M-030", -30.0F, 0.0F, false},
        {"M+000", 0.0F, 0.0F, false},
        {"LFE1", 0.0F, 0.0F, true},
    }};

    bool ok = true;
    auto alias_matrix = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1",
      "output_layout":"5.1",
      "routes":[{"source_label":"c","targets":[{"label":"l","weight":1}]}]
    })json");
    ok &= check(alias_matrix.has_value(), "alias matrix parses");
    if (alias_matrix) {
        auto resolved = mradm::render_common::resolve_direct_speakers_matrix_targets(*alias_matrix, targets, "0+5+0");
        ok &= check(resolved.has_value(), "case-insensitive DAW aliases resolve");
        if (resolved) {
            ok &= check(resolved->routes[0].source_key == "M+000", "source alias resolves to BS.2051 label");
            ok &= check(resolved->routes[0].targets[0].output_channel == 0U, "target alias resolves to output channel");
        }
    }

    const auto duplicate_source = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[
        {"source_label":"C","mute":true},{"source_label":"m_+000","mute":true}
      ]})json");
    if (duplicate_source) {
        const auto result =
            mradm::render_common::resolve_direct_speakers_matrix_targets(*duplicate_source, targets, "0+5+0");
        ok &= check_error(result, "duplicate", "duplicate source aliases are rejected");
    } else {
        ok &= check(false, "duplicate-source fixture parses before semantic resolution");
    }

    const auto duplicate_target = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[
        {"source_label":"C","targets":[{"label":"L","weight":1},{"label":"m_+030","weight":1}]}
      ]})json");
    if (duplicate_target) {
        const auto result =
            mradm::render_common::resolve_direct_speakers_matrix_targets(*duplicate_target, targets, "0+5+0");
        ok &= check_error(result, "duplicate", "duplicate target aliases are rejected");
    } else {
        ok &= check(false, "duplicate-target fixture parses before semantic resolution");
    }

    const auto unknown_target = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[
        {"source_label":"C","targets":[{"label":"U+180","weight":1}]}
      ]})json");
    if (unknown_target) {
        const auto result =
            mradm::render_common::resolve_direct_speakers_matrix_targets(*unknown_target, targets, "0+5+0");
        ok &= check_error(result, "not present", "target absent from output layout is rejected");
    } else {
        ok &= check(false, "unknown-target fixture parses before layout resolution");
    }

    const auto lfe_source = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[
        {"source_label":"RC_LFE","mute":true}
      ]})json");
    if (lfe_source) {
        const auto result = mradm::render_common::resolve_direct_speakers_matrix_targets(*lfe_source, targets, "0+5+0");
        ok &= check_error(result, "is LFE", "LFE source row is rejected");
    } else {
        ok &= check(false, "LFE-source fixture parses before semantic resolution");
    }

    const auto lfe_target = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[
        {"source_label":"C","targets":[{"label":"LFE","weight":1}]}
      ]})json");
    if (lfe_target) {
        const auto result = mradm::render_common::resolve_direct_speakers_matrix_targets(*lfe_target, targets, "0+5+0");
        ok &= check_error(result, "is LFE", "LFE target is rejected");
    } else {
        ok &= check(false, "LFE-target fixture parses before semantic resolution");
    }

    constexpr std::array<DirectSpeakerRoutingTarget, 2> ambiguous_targets{{
        {"M+030", 30.0F, 0.0F, false},
        {"L", 30.0F, 0.0F, false},
    }};
    if (alias_matrix) {
        const auto result = mradm::render_common::resolve_direct_speakers_matrix_targets(
            *alias_matrix, ambiguous_targets, "ambiguous-test-layout");
        ok &= check_error(result, "ambiguous", "ambiguous output label aliases are rejected");
    }

    const auto two_sources = mradm::parse_direct_speakers_matrix(R"json({
      "schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1","routes":[
        {"source_label":"C","mute":true},{"source_label":"L","mute":true}
      ]})json");
    if (two_sources) {
        const auto resolved =
            mradm::render_common::resolve_direct_speakers_matrix_targets(*two_sources, targets, "0+5+0");
        if (resolved) {
            mradm::SceneDirectSpeakersBlock block;
            block.speaker_labels = {"M+000", "M+030"};
            const auto result = mradm::render_common::direct_speakers_matrix_route_for_block(*resolved, block);
            ok &= check_error(result, "multiple", "block matching two source rows is rejected");
        } else {
            ok &= check(false, "two-source fixture resolves before block ambiguity check");
        }
    } else {
        ok &= check(false, "two-source fixture parses before block ambiguity check");
    }
    return ok;
}

} // namespace

int main() {
    const bool ok =
        verify_parser_and_coefficients() && verify_strict_schema_errors() && verify_target_and_source_resolution();
    if (ok) {
        std::cout << "DirectSpeakers matrix test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
