#include <utility>

#include <fmt/format.h>

#include "adm/render.h"

namespace mradm {

RenderService::RenderService() = default;

RenderResult RenderService::render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const {
    progress.on_progress({RenderStage::validating, 0.0, "validating request"});

    if (request.input_path.empty()) {
        return {{ErrorCode::invalid_argument, "input path is required", {}}, std::nullopt, {}};
    }

    logs.log(LogLevel::info, "render", fmt::format("received render request for {}", request.input_path.string()));
    progress.on_progress({RenderStage::probing, 0.1, "probing input"});

    RenderResult result;
    result.error = {
        ErrorCode::unsupported, "rendering is not implemented yet; next milestone is ADM BWF probe and scene dump", {}};
    result.output_path = request.output_path;
    result.diagnostics.push_back({LogLevel::warning, "C++ core skeleton is initialized"});

    progress.on_progress({RenderStage::finished, 1.0, "finished skeleton run"});
    return result;
}

} // namespace mradm
