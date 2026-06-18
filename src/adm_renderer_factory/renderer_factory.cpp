#include "renderer_factory.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

namespace mradm {

std::string normalize_output_layout(const std::string& layout) {
    std::string key = layout;
    std::ranges::transform(
        key, key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (key.empty() || key == "stereo" || key == "2.0" || key == "0+2+0") {
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

    // Speaker stereo rendering is intentionally not exposed: the current 2ch speaker
    // projection is not a downmix and can be badly misleading for ADM content.
    // Automatic 2ch output therefore means binaural.
    auto sel = requested;
    if (sel == RendererSelection::automatic && (requests_speaker_stereo || requested_layout == "binaural")) {
        sel = RendererSelection::saf_binaural;
    }
    if ((sel == RendererSelection::ear || sel == RendererSelection::saf) && requests_speaker_stereo &&
        !internal_allow_speaker_stereo) {
        return make_error(ErrorCode::unsupported,
                          "speaker stereo rendering is disabled; use --renderer saf-binaural for 2ch ADM output");
    }

    std::unique_ptr<IRenderer> renderer;
    if (sel == RendererSelection::ear || sel == RendererSelection::automatic) {
        renderer = create_ear_renderer();
    } else if (sel == RendererSelection::saf) {
        renderer = create_vbap_renderer();
    } else if (sel == RendererSelection::hoa) {
        renderer = create_hoa_renderer();
    } else if (sel == RendererSelection::binaural || sel == RendererSelection::saf_binaural) {
        renderer = create_binaural_renderer();
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
    resolved.effective_output_layout = requested_layout;

    if (sel == RendererSelection::binaural) {
        resolved.diagnostics.emplace_back(
            LogLevel::warning,
            "--renderer binaural is a legacy alias for --renderer saf-binaural; prefer saf-binaural");
    }
    if (sel == RendererSelection::binaural || sel == RendererSelection::saf_binaural) {
        if (requested_layout != "0+2+0" && requested_layout != "binaural") {
            resolved.diagnostics.emplace_back(
                LogLevel::warning,
                fmt::format("SAF binaural renderer always writes 2ch HRTF output; ignoring requested layout '{}'",
                            requested_layout));
        }
        resolved.effective_output_layout = "binaural";
    }

    resolved.renderer = std::move(renderer);
    return resolved;
}

} // namespace mradm
