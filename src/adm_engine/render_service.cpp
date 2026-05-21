#include <cstdlib>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/loudness.h"
#include "adm/options.h"
#include "adm/peak.h"
#include "adm/render.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
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
    for (const auto& w : scene_result->import_warnings) {
        logs.log(LogLevel::warning, "importer", w);
    }

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
    } else if (sel == RendererSelection::hoa) {
        renderer = create_hoa_renderer();
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
    plan.default_interp_ms = request.options.default_interp_ms;
    plan.scene = std::move(*scene_result);

    // Render.
    auto render_res = renderer->render(plan, progress, logs);
    if (!render_res) {
        return {render_res.error(), std::nullopt, {{LogLevel::error, render_res.error().message}}};
    }

    // Post-process: loudness normalisation first (may raise gain), then clamp True Peak.
    if (request.options.measure_loudness) {
        auto lufs_res = apply_loudness_norm(output_path, request.options.loudness_target_lufs, logs);
        if (!lufs_res) {
            return {lufs_res.error(), std::nullopt, {{LogLevel::error, lufs_res.error().message}}};
        }
    }

    // Post-process: True Peak limiting.
    if (request.options.peak_limit) {
        auto limit_res = apply_peak_limit(output_path, request.options.peak_limit_dbtp, logs);
        if (!limit_res) {
            return {limit_res.error(), std::nullopt, {{LogLevel::error, limit_res.error().message}}};
        }
    }

    // Final bit depth conversion (after all post-processing).
    if (request.options.output_bit_depth != OutputBitDepth::f32) {
        const uint16_t depth = (request.options.output_bit_depth == OutputBitDepth::i16) ? 16U : 24U;
        logs.log(LogLevel::info, "engine", fmt::format("converting to {}-bit integer PCM", depth));
        auto conv_res = audio::downconvert_to_int(output_path, depth);
        if (!conv_res) {
            return {conv_res.error(), std::nullopt, {{LogLevel::error, conv_res.error().message}}};
        }
    }

    return {{ErrorCode::ok, "", {}}, std::filesystem::path{output_path}, {{LogLevel::info, "render completed"}}};
}

} // namespace mradm
