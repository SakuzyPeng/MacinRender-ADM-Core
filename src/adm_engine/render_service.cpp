#include <cstdlib>

#include <fmt/format.h>

#include "adm/io.h"
#include "adm/render.h"
#include "adm/render_ear.h"
#include "adm/render_vbap.h"

namespace mradm {

RenderService::RenderService() = default;

RenderResult RenderService::render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const {
    progress.on_progress({RenderStage::validating, 0.0, "validating request"});

    if (request.input_path.empty()) {
        return {{ErrorCode::invalid_argument, "input path is required", {}}, std::nullopt, {}};
    }

    logs.log(LogLevel::info, "engine", fmt::format("render request: {}", request.input_path.string()));

    // Probe input for early error detection and logging.
    progress.on_progress({RenderStage::probing, 0.05, "probing input"});
    auto scene_result = io::import_scene(request.input_path.string());
    if (!scene_result) {
        return {scene_result.error(), std::nullopt, {{LogLevel::error, scene_result.error().message}}};
    }
    logs.log(LogLevel::info,
             "engine",
             fmt::format("scene: {} programmes, {} objects, {} ch @ {}Hz",
                         scene_result->programmes.size(),
                         scene_result->objects.size(),
                         scene_result->info.num_channels,
                         scene_result->info.sample_rate));

    // Resolve output path.
    std::string output_path;
    if (request.output_path.has_value() && !request.output_path->empty()) {
        output_path = request.output_path->string();
    } else {
        const auto stem = request.input_path.stem().string();
        const auto dir = request.input_path.parent_path();
        output_path = (dir / (stem + "_rendered.wav")).string();
    }

    // Select backend.
    const auto sel = request.options.renderer;
    std::unique_ptr<IRenderer> renderer;
    if (sel == RendererSelection::ear || sel == RendererSelection::automatic) {
        renderer = create_ear_renderer();
    } else if (sel == RendererSelection::saf) {
        renderer = create_vbap_renderer();
    } else {
        const auto msg = fmt::format("renderer '{}' is not available in this build", static_cast<int>(sel));
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, {{LogLevel::error, msg}}};
    }

    const auto caps = renderer->capabilities();
    logs.log(LogLevel::info, "engine", fmt::format("backend: {} {}", caps.backend_name, caps.backend_version));

    // Build plan.
    RenderPlan plan;
    plan.input_path = request.input_path.string();
    plan.output_path = output_path;
    plan.output_layout = request.options.output_layout.empty() ? "0+2+0" : request.options.output_layout;
    plan.scene = std::move(*scene_result);

    // Render.
    auto render_res = renderer->render(plan, progress, logs);
    if (!render_res) {
        return {render_res.error(), std::nullopt, {{LogLevel::error, render_res.error().message}}};
    }

    return {{ErrorCode::ok, "", {}}, std::filesystem::path{output_path}, {{LogLevel::info, "render completed"}}};
}

} // namespace mradm
