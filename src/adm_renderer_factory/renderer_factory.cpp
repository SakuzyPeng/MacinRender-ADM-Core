#include "renderer_factory.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "adm/direct_speakers_matrix.h"
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

#include "render_common.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

namespace mradm {

std::string normalize_output_layout(const std::string& layout) {
    std::string key = layout;
    std::ranges::transform(
        key, key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (key.empty()) {
        return "binaural";
    }
    if (key == "stereo" || key == "2.0" || key == "0+2+0") {
        return "0+2+0";
    }
    if (key == "5.1" || key == "0+5+0") {
        return "0+5+0";
    }
    if (key == "5.1.2" || key == "2+5+0") {
        return "2+5+0";
    }
    if (key == "7.1" || key == "wav71" || key == "wave_7_1" || key == "wave-7.1" || key == "0+7+0") {
        return "wav71";
    }
    if (key == "5.1.4" || key == "atmos514" || key == "4+5+0") {
        return "4+5+0";
    }
    if (key == "9.1.4" || key == "4+5+4") {
        return "4+5+4";
    }
    if (key == "7.1.4" || key == "atmos714" || key == "4+7+0") {
        return "4+7+0";
    }
    if (key == "9.1.6" || key == "atmos916") {
        return "9.1.6";
    }
    if (key == "22.2" || key == "9+10+3") {
        return "9+10+3";
    }
    if (key == "binaural" || key == "hoa3") {
        return key;
    }
    return layout;
}

Result<ResolvedRenderer>
resolve_renderer(RendererSelection requested, std::string requested_layout, bool internal_allow_speaker_stereo) {
    const bool requests_speaker_stereo = (requested_layout == "0+2+0");

    // Two-channel loudspeaker rendering is intentionally not exposed: the current 2ch speaker
    // projection is not a downmix and can be badly misleading for ADM content.
    // Automatic 2ch output therefore means binaural.
    auto sel = requested;
    if (sel == RendererSelection::automatic && (requests_speaker_stereo || requested_layout == "binaural")) {
        sel = RendererSelection::saf_binaural;
    }
    if ((sel == RendererSelection::ear || sel == RendererSelection::saf) && requests_speaker_stereo &&
        !internal_allow_speaker_stereo) {
        return make_error(ErrorCode::unsupported,
                          "two-channel loudspeaker rendering is unavailable; use binaural output instead");
    }

    std::unique_ptr<IRenderer> renderer;
    RendererSelection backend = sel;
    if (sel == RendererSelection::ear || sel == RendererSelection::automatic) {
        renderer = create_ear_renderer();
        backend = RendererSelection::ear;
    } else if (sel == RendererSelection::saf) {
        renderer = create_vbap_renderer();
    } else if (sel == RendererSelection::hoa) {
        renderer = create_hoa_renderer();
    } else if (sel == RendererSelection::binaural || sel == RendererSelection::saf_binaural) {
        renderer = create_binaural_renderer();
        backend = RendererSelection::saf_binaural;
#ifdef __APPLE__
    } else if (sel == RendererSelection::apple) {
        renderer = create_apple_renderer();
#endif
    } else {
        return make_error(ErrorCode::unsupported,
                          fmt::format("renderer '{}' is not available in this build", static_cast<int>(sel)));
    }

    ResolvedRenderer resolved;
    resolved.selected = sel;
    resolved.backend = backend;
    resolved.effective_output_layout = requested_layout;

    if (sel == RendererSelection::binaural) {
        resolved.diagnostics.emplace_back(
            LogLevel::warning,
            "--renderer binaural is a legacy alias for --renderer saf-binaural; prefer saf-binaural");
    }
    if (sel == RendererSelection::binaural || sel == RendererSelection::saf_binaural) {
        if (requested_layout != "0+2+0" && requested_layout != "binaural") {
            return make_error(ErrorCode::unsupported,
                              fmt::format("SAF binaural backend does not support output layout '{}'", requested_layout),
                              "layout=" + requested_layout);
        }
        resolved.effective_output_layout = "binaural";
    }

    resolved.renderer = std::move(renderer);
    return resolved;
}

Result<void> validate_direct_speakers_routing(RendererSelection backend,
                                              std::string_view effective_output_layout,
                                              DirectSpeakersRoutingMode mode) {
    if (mode == DirectSpeakersRoutingMode::automatic) {
        return {};
    }
    const bool binaural = effective_output_layout == "0+2+0" || effective_output_layout == "binaural";
    if (mode == DirectSpeakersRoutingMode::matrix) {
        // EAR and SAF identify loudspeaker backends directly; the internal
        // 0+2+0 speaker test layout must not be mistaken for binaural output.
        if (backend == RendererSelection::ear || backend == RendererSelection::saf) {
            return {};
        }
        if (backend == RendererSelection::apple && !binaural) {
            return {};
        }
        return make_error(ErrorCode::unsupported,
                          "DirectSpeakers matrix routing is supported only by EAR, SAF, and Apple speaker outputs");
    }
    if (backend == RendererSelection::saf) {
        return {};
    }
    if (backend == RendererSelection::apple) {
        if (binaural && mode == DirectSpeakersRoutingMode::label) {
            return make_error(ErrorCode::unsupported,
                              "Apple binaural output does not support DirectSpeakers label routing; use position");
        }
        return {};
    }
    return make_error(ErrorCode::unsupported,
                      "explicit DirectSpeakers routing is supported only by SAF and Apple renderers");
}

namespace {

using MatrixSourceRows = std::unordered_map<std::string, std::size_t>;

[[nodiscard]] Result<MatrixSourceRows> validate_matrix_route_labels(const DirectSpeakersMatrix& matrix) {
    MatrixSourceRows source_rows;
    source_rows.reserve(matrix.routes.size());
    for (std::size_t route_index = 0; route_index < matrix.routes.size(); ++route_index) {
        const auto& route = matrix.routes[route_index];
        const std::string source_key = render_common::canonical_direct_speaker_label(route.source_label);
        if (source_key.empty()) {
            return make_error(ErrorCode::invalid_argument,
                              "DirectSpeakers matrix source_label resolves to an empty label",
                              "source_label=" + route.source_label);
        }
        if (render_common::is_lfe_label(source_key)) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("DirectSpeakers matrix source '{}' is LFE; LFE uses dedicated routing",
                                          route.source_label));
        }
        if (!source_rows.emplace(source_key, route_index).second) {
            return make_error(
                ErrorCode::invalid_argument,
                fmt::format("duplicate DirectSpeakers matrix source '{}' after alias resolution", route.source_label));
        }

        std::set<std::string> target_keys;
        for (const auto& target : route.targets) {
            const std::string target_key = render_common::canonical_direct_speaker_label(target.label);
            if (target_key.empty()) {
                return make_error(ErrorCode::invalid_argument,
                                  "DirectSpeakers matrix target label resolves to an empty label",
                                  "source_label=" + route.source_label);
            }
            if (render_common::is_lfe_label(target_key)) {
                return make_error(
                    ErrorCode::invalid_argument,
                    fmt::format("DirectSpeakers matrix target '{}' is LFE; LFE uses dedicated routing", target.label),
                    "source_label=" + route.source_label);
            }
            if (!target_keys.insert(target_key).second) {
                return make_error(
                    ErrorCode::invalid_argument,
                    fmt::format("duplicate DirectSpeakers matrix target '{}' after alias resolution", target.label),
                    "source_label=" + route.source_label);
            }
        }
    }
    return source_rows;
}

[[nodiscard]] Result<void> validate_matrix_scene_coverage(const DirectSpeakersMatrix& matrix,
                                                          const MatrixSourceRows& source_rows,
                                                          const AdmScene& scene,
                                                          LogSink& logs) {
    std::vector<bool> used(matrix.routes.size(), false);
    for (const auto& object : scene.objects) {
        for (const auto& track : object.tracks) {
            for (const auto& block : track.ds_blocks) {
                if (render_common::direct_speakers_block_is_lfe(block)) {
                    continue;
                }
                std::optional<std::size_t> matched;
                for (const auto& label : block.speaker_labels) {
                    const auto row = source_rows.find(render_common::canonical_direct_speaker_label(label));
                    if (row == source_rows.end()) {
                        continue;
                    }
                    if (matched.has_value() && *matched != row->second) {
                        return make_error(ErrorCode::invalid_argument,
                                          "DirectSpeakers block matches multiple matrix source rows",
                                          fmt::format("track_uid={} labels={}",
                                                      track.track_uid,
                                                      fmt::join(block.speaker_labels, ",")));
                    }
                    matched = row->second;
                }
                if (!matched.has_value()) {
                    const std::string labels = block.speaker_labels.empty()
                                                   ? std::string{"<missing>"}
                                                   : fmt::format("{}", fmt::join(block.speaker_labels, ","));
                    return make_error(
                        ErrorCode::invalid_argument,
                        fmt::format("DirectSpeakers block label '{}' is not covered by the routing matrix", labels),
                        "track_uid=" + track.track_uid);
                }
                used[*matched] = true;
            }
        }
    }

    for (std::size_t route_index = 0; route_index < matrix.routes.size(); ++route_index) {
        if (!used[route_index]) {
            logs.log(LogLevel::warning,
                     "direct-speakers-matrix",
                     fmt::format("matrix source '{}' is not present in this scene",
                                 matrix.routes[route_index].source_label));
        }
    }
    return {};
}

} // namespace

Result<std::shared_ptr<const DirectSpeakersMatrix>> resolve_direct_speakers_matrix(
    const RenderOptions& options, const AdmScene& scene, std::string_view effective_output_layout, LogSink& logs) {
    const bool has_path = options.direct_speakers_matrix_path.has_value();
    const bool has_json = options.direct_speakers_matrix_json.has_value();
    if (options.direct_speakers_routing_mode != DirectSpeakersRoutingMode::matrix) {
        if (has_path || has_json) {
            return make_error(ErrorCode::invalid_argument,
                              "DirectSpeakers matrix configuration requires routing mode 'matrix'");
        }
        return std::shared_ptr<const DirectSpeakersMatrix>{};
    }
    if (!has_path && !has_json) {
        return make_error(ErrorCode::invalid_argument,
                          "DirectSpeakers matrix routing requires a matrix path or in-memory JSON");
    }

    Result<DirectSpeakersMatrix> loaded = has_json
                                              ? parse_direct_speakers_matrix(*options.direct_speakers_matrix_json)
                                              : load_direct_speakers_matrix_file(*options.direct_speakers_matrix_path);
    if (has_json && has_path) {
        logs.log(LogLevel::warning,
                 "direct-speakers-matrix",
                 "in-memory DirectSpeakers matrix supplied; ignoring direct_speakers_matrix_path");
    }
    if (!loaded) {
        return tl::unexpected{loaded.error()};
    }

    const std::string matrix_layout = normalize_output_layout(loaded->output_layout);
    if (matrix_layout != effective_output_layout) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("DirectSpeakers matrix layout '{}' does not match output layout '{}'",
                                      loaded->output_layout,
                                      effective_output_layout));
    }

    auto source_rows = validate_matrix_route_labels(*loaded);
    if (!source_rows) {
        return tl::unexpected{source_rows.error()};
    }
    auto coverage = validate_matrix_scene_coverage(*loaded, *source_rows, scene, logs);
    if (!coverage) {
        return tl::unexpected{coverage.error()};
    }
    return std::make_shared<const DirectSpeakersMatrix>(std::move(*loaded));
}

} // namespace mradm
