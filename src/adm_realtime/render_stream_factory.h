#pragma once

#include <memory>

#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/render.h"
#include "adm/scene.h"

// Opens an IRenderStream for a scene + options. Injected into the MonitorEngine so the
// engine's plumbing (ring / worker / clock / loop / seek) can be unit-tested against a
// deterministic test stream with no real backend (a PatternStreamFactory), while the
// production path uses RealtimeStreamFactory (resolve_renderer + prepare + open_stream).
// See REALTIME_MONITORING_SLICE1.md §4.1.
namespace mradm::realtime {

class IRenderStreamFactory {
  public:
    virtual ~IRenderStreamFactory() = default;
    IRenderStreamFactory(const IRenderStreamFactory&) = delete;
    IRenderStreamFactory& operator=(const IRenderStreamFactory&) = delete;
    IRenderStreamFactory(IRenderStreamFactory&&) = delete;
    IRenderStreamFactory& operator=(IRenderStreamFactory&&) = delete;

    // Build a streaming session for `scene` under `opts`. Returns ErrorCode::unsupported
    // when the resolved backend has no realtime stream (default IRenderer::open_stream).
    [[nodiscard]] virtual Result<std::unique_ptr<IRenderStream>>
    open(const AdmScene& scene, const RenderOptions& opts, LogSink& logs) = 0;

  protected:
    IRenderStreamFactory() = default;
};

} // namespace mradm::realtime
