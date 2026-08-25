#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/render.h"

// Internal backend-resolution helper, shared by RenderService (offline) and the
// realtime MonitorEngine so the two never diverge on backend selection. This is an
// internal library (MacinRender::ADMRendererFactory): it knows every backend
// (create_*_renderer) and is deliberately kept OUT of ADMCore / public headers so the
// core domain layer does not turn into a backend registry. See
// docs/architecture/REALTIME_MONITORING_SLICE1.md §5.
namespace mradm {

// The full result of resolving a requested renderer + output layout: the constructed
// backend, the selection for *reporting*, the effective output layout (e.g. SAF binaural
// forces "binaural"), and any user-facing diagnostics the caller should log in order.
// Resolution emits no logs itself; the caller decides placement (so it can interleave its
// own "backend:" line).
//
// `renderer` is the canonical handle — dispatch off it, not off `selected`. `selected`
// mirrors what RenderService historically reported via renderer_name(): the automatic→
// saf_binaural promotion for 2ch is applied, but automatic (non-stereo) stays automatic
// and the legacy "binaural" alias is preserved as-is (with a diagnostic warning). It is
// NOT a canonical backend id; two distinct `selected` values can map to one backend.
struct ResolvedRenderer {
    std::unique_ptr<IRenderer> renderer;
    RendererSelection selected{RendererSelection::automatic};
    // Concrete backend selected for dispatch/feature validation. Unlike
    // `selected`, automatic speaker output resolves to `ear` here.
    RendererSelection backend{RendererSelection::automatic};
    std::string effective_output_layout;
    std::vector<std::pair<LogLevel, std::string>> diagnostics;
};

// Resolve `requested` + an already-normalised `requested_layout` into a backend.
// Mirrors the behaviour previously inlined in RenderService::render:
//   - automatic + "0+2+0"/"binaural" output  -> saf_binaural
//   - speaker stereo ("0+2+0") on ear/saf is rejected unless internal_allow_speaker_stereo
//   - the "binaural" selection is a legacy alias for saf_binaural (diagnostic warning)
//   - saf binaural always writes 2ch HRTF -> effective layout forced to "binaural"
//   - apple is unavailable on non-Apple builds
// Returns ErrorCode::unsupported for the rejected / unavailable cases.
[[nodiscard]] Result<ResolvedRenderer>
resolve_renderer(RendererSelection requested, std::string requested_layout, bool internal_allow_speaker_stereo);

// Validate an explicit DirectSpeakers routing request after backend/layout
// resolution. Automatic preserves native behaviour on unsupported backends.
[[nodiscard]] Result<void> validate_direct_speakers_routing(RendererSelection backend,
                                                            std::string_view effective_output_layout,
                                                            DirectSpeakersRoutingMode mode);

// Resolve path/JSON precedence, validate the matrix against the effective scene
// and output layout, and return an immutable profile shared by offline and
// realtime plans. A null shared_ptr means no matrix was requested.
[[nodiscard]] Result<std::shared_ptr<const DirectSpeakersMatrix>> resolve_direct_speakers_matrix(
    const RenderOptions& options, const AdmScene& scene, std::string_view effective_output_layout, LogSink& logs);

// Map a requested output-layout string (case-insensitive aliases like "5.1" / "atmos714" /
// "stereo") to its canonical layout id ("0+5+0" / "4+7+0" / "0+2+0" / "binaural" / "hoa3"…).
// Unknown values pass through unchanged. Shared by RenderService and the realtime monitor so
// both accept the same aliases.
[[nodiscard]] std::string normalize_output_layout(const std::string& layout);

} // namespace mradm
