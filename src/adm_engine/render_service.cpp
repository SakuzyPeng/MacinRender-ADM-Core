#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/options.h"
#include "adm/render.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

namespace mradm {


RenderService::RenderService() = default;

RenderResult RenderService::render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const {
    progress.on_progress({RenderStage::validating, 0.0, "validating request"});

    if (request.input_path.empty()) {
        return {{ErrorCode::invalid_argument, "input path is required", {}}, std::nullopt, std::nullopt, {}};
    }

    logs.log(LogLevel::info, "engine", fmt::format("render request: {}", request.input_path.string()));

    // Probe input for early error detection and logging.
    progress.on_progress({RenderStage::probing, 0.05, "probing input"});
    auto scene_result = io::import_scene(request.input_path.string());
    if (!scene_result) {
        return {scene_result.error(), std::nullopt, std::nullopt, {{LogLevel::error, scene_result.error().message}}};
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
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }

    const auto caps = renderer->capabilities();
    logs.log(LogLevel::info, "engine", fmt::format("backend: {} {}", caps.backend_name, caps.backend_version));

    const auto output_layout = request.options.output_layout.empty() ? "0+2+0" : request.options.output_layout;

    // Build plan.
    RenderPlan plan;
    plan.input_path = request.input_path.string();
    plan.output_path = output_path;
    plan.output_layout = output_layout;
    plan.default_interp_ms = request.options.default_interp_ms;
    plan.scene = std::move(*scene_result);

    // Render (inline measurement of loudness + True Peak).
    auto render_res = renderer->render(plan, progress, logs);
    if (!render_res) {
        return {render_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, render_res.error().message}}};
    }
    const RenderMetrics& metrics = *render_res;

    if (metrics.measured_lufs) {
        logs.log(LogLevel::info, "engine", fmt::format("measured loudness: {:.1f} LUFS", *metrics.measured_lufs));
    }
    if (metrics.measured_peak_dbtp) {
        logs.log(
            LogLevel::info, "engine", fmt::format("measured true peak: {:.2f} dBTP", *metrics.measured_peak_dbtp));
    }

    // Compute combined gain: loudness target first, then peak ceiling.
    // Merging both into one apply_gain_to_file avoids a second read-write pass.
    double gain_db = 0.0;

    if (request.options.measure_loudness && metrics.measured_lufs.has_value()) {
        const double target = static_cast<double>(request.options.loudness_target_lufs);
        const double delta  = target - *metrics.measured_lufs;
        if (std::abs(delta) >= 0.1) {
            gain_db += delta;
            logs.log(LogLevel::info,
                     "engine",
                     fmt::format("loudness target {:.1f} LUFS → gain {:.2f} dB", target, delta));
        } else {
            logs.log(LogLevel::info, "engine", "integrated loudness within 0.1 LU of target — no adjustment");
        }
    }

    if (request.options.peak_limit && metrics.measured_peak_dbtp.has_value()) {
        const double peak_after = *metrics.measured_peak_dbtp + gain_db;
        const double target_peak = static_cast<double>(request.options.peak_limit_dbtp);
        const double peak_clamp  = std::min(0.0, target_peak - peak_after);
        if (peak_clamp < -0.1) {
            gain_db += peak_clamp;
            logs.log(LogLevel::info,
                     "engine",
                     fmt::format("true peak after loudness {:.2f} dBTP, ceiling {:.1f} dBTP → clamp {:.2f} dB",
                                 peak_after,
                                 target_peak,
                                 peak_clamp));
        } else {
            logs.log(LogLevel::info, "engine", "true peak within target — no clamp");
        }
    }

    if (std::abs(gain_db) >= 0.01) {
        const float gain_linear = static_cast<float>(std::pow(10.0, gain_db / 20.0));
        logs.log(LogLevel::info,
                 "engine",
                 fmt::format("applying total gain {:.4f} ({:.2f} dB)", gain_linear, gain_db));
        auto gain_res = audio::apply_gain_to_file(output_path, gain_linear, output_layout);
        if (!gain_res) {
            return {gain_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, gain_res.error().message}}};
        }
    }

    // Final bit depth conversion (after all post-processing). CAF is always float32.
    if (request.options.output_bit_depth != OutputBitDepth::f32) {
        const uint16_t depth = (request.options.output_bit_depth == OutputBitDepth::i16) ? 16U : 24U;
        logs.log(LogLevel::info, "engine", fmt::format("converting to {}-bit integer PCM", depth));
        auto conv_res = audio::downconvert_to_int(output_path, depth);
        if (!conv_res) {
            return {conv_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, conv_res.error().message}}};
        }
    }

    return {{ErrorCode::ok, "", {}},
            std::filesystem::path{output_path},
            metrics,
            {{LogLevel::info, "render completed"}}};
}

} // namespace mradm
