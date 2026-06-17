#pragma once

#include <memory>
#include <string>
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
// backend, the renderer actually selected (automatic / legacy aliases normalised), the
// effective output layout (e.g. SAF binaural forces "binaural"), and any user-facing
// diagnostics the caller should log in order. Resolution emits no logs itself; the
// caller decides placement (so it can interleave its own "backend:" line).
struct ResolvedRenderer {
    std::unique_ptr<IRenderer> renderer;
    RendererSelection selected{RendererSelection::automatic};
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

} // namespace mradm
