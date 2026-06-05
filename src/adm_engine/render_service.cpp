#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/options.h"
#include "adm/render.h"
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"
#include "adm/semantic_policy.h"

#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

#include "capability_json.h"
#include "format_table.h"
#include "layout_table.h"
#include "scene_json.h"

namespace mradm {

namespace {

[[nodiscard]] std::tm utc_time(std::time_t t) {
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    return tm_utc;
}

[[nodiscard]] std::filesystem::path unique_sidecar_temp_path(const std::filesystem::path& final_path,
                                                             std::string_view purpose,
                                                             std::string_view extension) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;

    const auto parent = final_path.parent_path();
    const auto stem = final_path.stem().string();
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto candidate = parent / fmt::format("{}.{}.{:016x}{}", stem, purpose, dist(rng), extension);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return parent / fmt::format("{}.{}.{:016x}{}", stem, purpose, dist(rng), extension);
}

[[nodiscard]] std::filesystem::path unique_render_temp_path(const std::filesystem::path& final_path) {
    return unique_sidecar_temp_path(final_path, "render_tmp", ".wav");
}

[[nodiscard]] std::filesystem::path unique_output_temp_path(const std::filesystem::path& final_path) {
    const auto ext = final_path.extension().string();
    return unique_sidecar_temp_path(final_path, "output_tmp", ext.empty() ? ".tmp" : ext);
}

[[nodiscard]] std::string normalize_output_layout(const std::string& layout) {
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

[[nodiscard]] std::optional<uint16_t> channel_count_for_layout(const CapabilityReport& caps,
                                                               std::string_view layout_id) {
    const auto it = std::ranges::find_if(
        caps.supported_layouts, [layout_id](const CapabilityReport::Layout& layout) { return layout.id == layout_id; });
    if (it != caps.supported_layouts.end()) {
        return it->channel_count;
    }

    if (layout_id == "binaural") {
        const auto binaural_it = std::ranges::find_if(
            caps.supported_layouts, [](const CapabilityReport::Layout& layout) { return layout.is_binaural; });
        if (binaural_it != caps.supported_layouts.end()) {
            return binaural_it->channel_count;
        }
    }

    return std::nullopt;
}

[[nodiscard]] bool flac_supports_layout(std::string_view layout_id) {
    return layout_id == "binaural" || layout_id == "0+2+0" || layout_id == "0+5+0" || layout_id == "wav71";
}

[[nodiscard]] std::string renderer_name(RendererSelection sel) {
    switch (sel) {
    case RendererSelection::automatic:
        return "auto";
    case RendererSelection::ear:
        return "ear";
    case RendererSelection::saf:
        return "saf";
    case RendererSelection::hoa:
        return "hoa";
    case RendererSelection::apple:
        return "apple";
    case RendererSelection::binaural:
        return "binaural";
    case RendererSelection::saf_binaural:
        return "saf-binaural";
    }
    return "unknown";
}

[[nodiscard]] std::string apple_spatial_preset_name(AppleSpatialPreset preset) {
    switch (preset) {
    case AppleSpatialPreset::off:
        return "off";
    case AppleSpatialPreset::headphone_default:
        return "headphone-default";
    case AppleSpatialPreset::headphone_movie:
        return "headphone-movie";
    }
    return "unknown";
}

[[nodiscard]] double clamp_progress(double value) {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 1.0);
}

void emit_progress(ProgressSink& progress,
                   RenderStage stage,
                   RenderOperation operation,
                   double fraction,
                   double stage_fraction,
                   std::string_view message,
                   uint64_t current_frame = 0,
                   uint64_t total_frames = 0) {
    progress.on_progress({stage,
                          operation,
                          clamp_progress(fraction),
                          clamp_progress(stage_fraction),
                          current_frame,
                          total_frames,
                          message});
}

class ProgressRangeSink final : public ProgressSink {
  public:
    ProgressRangeSink(ProgressSink& downstream,
                      RenderStage stage,
                      RenderOperation operation,
                      double start,
                      double end,
                      std::string_view fallback_message)
        : downstream_(&downstream), stage_(stage), operation_(operation), start_(clamp_progress(start)),
          end_(clamp_progress(end)), fallback_message_(fallback_message) {}

    void on_progress(const ProgressEvent& event) override {
        const double local = (event.stage == RenderStage::finished) ? 1.0 : clamp_progress(event.stage_fraction);
        const double mapped = start_ + ((end_ - start_) * local);
        const auto operation = (event.operation == RenderOperation::unknown || event.stage == RenderStage::finished)
                                   ? operation_
                                   : event.operation;
        const auto message = event.message.empty() ? std::string_view{fallback_message_} : event.message;
        downstream_->on_progress(
            {stage_, operation, clamp_progress(mapped), local, event.current_frame, event.total_frames, message});
    }

  private:
    ProgressSink* downstream_;
    RenderStage stage_;
    RenderOperation operation_;
    double start_{0.0};
    double end_{1.0};
    std::string_view fallback_message_;
};

class TempFileGuard {
  public:
    explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    TempFileGuard(TempFileGuard&&) = delete;
    TempFileGuard& operator=(TempFileGuard&&) = delete;
    ~TempFileGuard() { remove_now(); }

    void remove_now() noexcept {
        if (!active_) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        active_ = false;
    }
    void dismiss() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

[[nodiscard]] Result<void> replace_output_file(const std::filesystem::path& temp_path,
                                               const std::filesystem::path& final_path) {
    std::error_code ec;
    std::filesystem::rename(temp_path, final_path, ec);
    if (!ec) {
        return {};
    }

    std::error_code remove_ec;
    std::filesystem::remove(final_path, remove_ec);
    ec.clear();
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        return make_error(
            ErrorCode::io_error, "failed to replace output file: " + ec.message(), "path=" + final_path.string());
    }
    return {};
}

class OutputCleanupGuard {
  public:
    explicit OutputCleanupGuard(std::filesystem::path path) : path_(std::move(path)) {}
    OutputCleanupGuard(const OutputCleanupGuard&) = delete;
    OutputCleanupGuard& operator=(const OutputCleanupGuard&) = delete;
    OutputCleanupGuard(OutputCleanupGuard&&) = delete;
    OutputCleanupGuard& operator=(OutputCleanupGuard&&) = delete;
    ~OutputCleanupGuard() { remove_now(); }

    void arm() noexcept { active_ = true; }
    void commit() noexcept { active_ = false; }

    void remove_now() noexcept {
        if (!active_) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        active_ = false;
    }

  private:
    std::filesystem::path path_;
    bool active_{false};
};

// Resolve the semantic policy from options (in-memory JSON preferred over a file
// path) and apply it to the scene in place. Returns the applied policy (for the
// report) or nullopt when no policy was requested. Shared by render() and
// prepare_preview_scene() so the json/path precedence stays in one place.
[[nodiscard]] Result<std::optional<SemanticPolicy>> resolve_and_apply_policy(AdmScene& scene,
                                                                             const RenderOptions& options,
                                                                             std::vector<std::string>& warnings,
                                                                             LogSink& logs) {
    std::optional<SemanticPolicy> policy;
    if (options.semantic_policy_json.has_value()) {
        if (options.semantic_policy_path.has_value()) {
            logs.log(LogLevel::warning,
                     "semantic-policy",
                     "in-memory semantic policy supplied; ignoring semantic_policy_path");
        }
        auto res = parse_semantic_policy(*options.semantic_policy_json, "<memory>");
        if (!res) {
            return tl::unexpected{res.error()};
        }
        policy = std::move(*res);
    } else if (options.semantic_policy_path.has_value()) {
        auto res = load_semantic_policy_file(*options.semantic_policy_path);
        if (!res) {
            return tl::unexpected{res.error()};
        }
        policy = std::move(*res);
    }
    if (policy.has_value()) {
        auto applied = apply_semantic_policy(scene, *policy, scene.info.sample_rate, &warnings);
        if (!applied) {
            return tl::unexpected{applied.error()};
        }
        for (const auto& warning : warnings) {
            logs.log(LogLevel::warning, "semantic-policy", warning);
        }
    }
    return policy;
}

} // anonymous namespace

RenderService::RenderService() = default;

// RenderService intentionally owns the full orchestration pipeline; splitting it
// mechanically would hide the ordering constraints between render, post-process,
// encode, and metadata.
// NOLINTNEXTLINE(readability-function-size)
RenderResult RenderService::render(const RenderRequest& request,
                                   ProgressSink& progress,
                                   LogSink& logs,
                                   const AdmScene* preimported_scene,
                                   std::shared_ptr<IPreparedRender>* prepared_cache) const {
    emit_progress(progress, RenderStage::validating, RenderOperation::validate_request, 0.0, 0.0, "validating request");

    if (request.input_path.empty()) {
        return {{ErrorCode::invalid_argument, "input path is required", {}}, std::nullopt, std::nullopt, {}};
    }

    logs.log(LogLevel::info, "engine", fmt::format("render request: {}", request.input_path.string()));

    if (request.options.peak_normalize_to_limit && !request.options.peak_limit) {
        const auto msg = std::string{"peak normalization to limit requires peak limiting to be enabled"};
        return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }
    if (!std::isfinite(request.options.final_gain_db)) {
        const auto msg = std::string{"final gain must be finite"};
        return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }

    // Probe input for early error detection and logging. A PreviewSession supplies an
    // already-imported, policy-applied scene (copied here); otherwise import from disk.
    emit_progress(progress, RenderStage::probing, RenderOperation::probe_input, 0.05, 0.0, "probing input");
    emit_progress(progress,
                  RenderStage::importing_scene,
                  RenderOperation::import_scene,
                  0.10,
                  0.0,
                  preimported_scene != nullptr ? "using cached scene" : "importing scene");
    Result<AdmScene> scene_result = (preimported_scene != nullptr) ? Result<AdmScene>{*preimported_scene}
                                                                   : io::import_scene(request.input_path.string());
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
    if (preimported_scene == nullptr) {
        for (const auto& w : scene_result->import_warnings) {
            logs.log(LogLevel::warning, "importer", w);
        }
    }

    if (request.options.cancel_token.stop_requested()) {
        constexpr auto msg = "render cancelled";
        return {{ErrorCode::cancelled, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::info, msg}}};
    }

    // Resolve the optional output time-range trim against the rendered timeline
    // (which equals the input timeline). We compute the frame range here, while
    // the scene info is still available, and apply it to the rendered PCM below.
    const uint32_t trim_sample_rate = scene_result->info.sample_rate;
    const uint64_t trim_total_frames = scene_result->info.num_frames;
    bool need_trim = false;
    uint64_t trim_start_frame = 0;
    uint64_t trim_frame_count = 0;
    {
        const double start_sec = request.options.render_start_sec;
        const bool has_end = request.options.render_end_sec.has_value();
        if (start_sec > 0.0 || has_end) {
            if (start_sec < 0.0) {
                const auto msg = fmt::format("--start must be >= 0 (got {:.6g}s)", start_sec);
                return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
            }
            const double end_sec = request.options.render_end_sec.value_or(0.0);
            if (has_end && end_sec <= start_sec) {
                const auto msg =
                    fmt::format("--end ({:.6g}s) must be greater than --start ({:.6g}s)", end_sec, start_sec);
                return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
            }
            if (trim_sample_rate == 0) {
                constexpr auto msg = "cannot apply time-range trim: input sample rate is unknown";
                return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
            }
            const auto sr = static_cast<double>(trim_sample_rate);
            trim_start_frame = static_cast<uint64_t>(std::llround(start_sec * sr));
            if (trim_start_frame >= trim_total_frames) {
                const auto msg = fmt::format("--start ({:.6g}s) is at or beyond the input duration ({:.6g}s)",
                                             start_sec,
                                             static_cast<double>(trim_total_frames) / sr);
                return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
            }
            uint64_t end_frame = has_end ? static_cast<uint64_t>(std::llround(end_sec * sr)) : trim_total_frames;
            end_frame = std::min(end_frame, trim_total_frames);
            // The seconds-level "--end > --start" check above can still collapse to
            // an empty frame range once rounded (e.g. both endpoints land in the same
            // frame). Reject it here, before rendering, so we neither waste a full
            // render nor leave an un-trimmed file for direct WAV/CAF outputs.
            if (end_frame <= trim_start_frame) {
                const auto msg = fmt::format("trim window [{:.6g}s, {:.6g}s) is shorter than one sample frame at {}Hz",
                                             start_sec,
                                             end_sec,
                                             trim_sample_rate);
                return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
            }
            trim_frame_count = end_frame - trim_start_frame;
            need_trim = true;
            logs.log(LogLevel::info,
                     "engine",
                     fmt::format("output trim: frames [{}, {}) of {} @ {}Hz",
                                 trim_start_frame,
                                 trim_start_frame + trim_frame_count,
                                 trim_total_frames,
                                 trim_sample_rate));
        }
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

    const auto requested_layout = normalize_output_layout(
        request.options.output_layout.empty() ? std::string{"0+2+0"} : request.options.output_layout);
    const bool requests_speaker_stereo = (requested_layout == "0+2+0");

    // Select backend. Speaker stereo rendering is intentionally not exposed:
    // the current 2ch speaker projection is not a downmix and can be badly
    // misleading for ADM content. Automatic 2ch output therefore means binaural.
    auto sel = request.options.renderer;
    if (sel == RendererSelection::automatic && (requests_speaker_stereo || requested_layout == "binaural")) {
        sel = RendererSelection::saf_binaural;
    }
    if ((sel == RendererSelection::ear || sel == RendererSelection::saf) && requests_speaker_stereo &&
        !request.options.internal_allow_speaker_stereo) {
        const auto msg =
            std::string{"speaker stereo rendering is disabled; use --renderer saf-binaural for 2ch ADM output"};
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
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
        const auto msg = fmt::format("renderer '{}' is not available in this build", static_cast<int>(sel));
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }

    const auto caps = renderer->capabilities();
    logs.log(LogLevel::info, "engine", fmt::format("backend: {} {}", caps.backend_name, caps.backend_version));
    if (sel == RendererSelection::binaural) {
        logs.log(LogLevel::warning,
                 "engine",
                 "--renderer binaural is a legacy alias for --renderer saf-binaural; prefer saf-binaural");
    }

    std::optional<SemanticPolicy> semantic_policy;
    std::vector<std::string> semantic_warnings;
    // A preview session has already imported + applied the policy; it does not
    // re-derive the semantic report per window.
    const bool needs_semantic_report =
        (preimported_scene == nullptr) &&
        (request.options.semantic_report_path.has_value() || request.options.capture_semantic_report);
    const AdmScene original_scene = needs_semantic_report ? *scene_result : AdmScene{};
    // Effective semantic report captured in-memory when requested; surfaced via RenderResult.
    std::optional<std::string> semantic_report_json;

    auto output_layout = requested_layout;
    if (sel == RendererSelection::binaural || sel == RendererSelection::saf_binaural) {
        if (requested_layout != "0+2+0" && requested_layout != "binaural") {
            logs.log(LogLevel::warning,
                     "engine",
                     fmt::format("SAF binaural renderer always writes 2ch HRTF output; ignoring requested layout '{}'",
                                 requested_layout));
        }
        output_layout = "binaural";
    }

    // Resolve + apply the semantic policy (skipped when a preview session already did).
    if (preimported_scene == nullptr) {
        emit_progress(progress,
                      RenderStage::importing_scene,
                      RenderOperation::apply_semantic_policy,
                      0.18,
                      0.0,
                      "applying semantic policy");
        auto policy_res = resolve_and_apply_policy(*scene_result, request.options, semantic_warnings, logs);
        if (!policy_res) {
            return {policy_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, policy_res.error().message}}};
        }
        semantic_policy = std::move(*policy_res);
    }

    // From here on the semantic report may be captured; every error path must carry
    // it back so the C ABI's adm_render_result_semantic_report_json stays readable
    // regardless of the render's error code (see include/adm/c_api.h). Captured by
    // reference; the success return moves it out afterwards, after the last call.
    const auto fail_with_report = [&semantic_report_json](Error error,
                                                          LogLevel level = LogLevel::error) -> RenderResult {
        const std::string message = error.message;
        return {std::move(error), std::nullopt, std::nullopt, {{level, message}}, semantic_report_json};
    };
    const auto fail_if_cancelled = [&request, &fail_with_report]() -> std::optional<RenderResult> {
        if (!request.options.cancel_token.stop_requested()) {
            return std::nullopt;
        }
        return fail_with_report({ErrorCode::cancelled, "render cancelled", {}}, LogLevel::info);
    };

    if (request.options.apple_spatial_preset != AppleSpatialPreset::off) {
        if (sel != RendererSelection::apple) {
            return fail_with_report({ErrorCode::invalid_argument,
                                     fmt::format("--apple-spatial-preset {} requires --renderer apple",
                                                 apple_spatial_preset_name(request.options.apple_spatial_preset)),
                                     {}});
        }
        if (output_layout != "0+2+0" && output_layout != "binaural") {
            return fail_with_report({ErrorCode::invalid_argument,
                                     fmt::format("--apple-spatial-preset {} requires Apple binaural output; got '{}'",
                                                 apple_spatial_preset_name(request.options.apple_spatial_preset),
                                                 output_layout),
                                     {}});
        }
    }

    if (needs_semantic_report) {
        const SemanticPolicyReportOptions report_options{
            .renderer = renderer_name(sel),
            .policy_path =
                request.options.semantic_policy_path ? request.options.semantic_policy_path->string() : std::string{},
            .capabilities = caps,
        };
        if (request.options.capture_semantic_report) {
            semantic_report_json = build_semantic_report(original_scene,
                                                         *scene_result,
                                                         semantic_policy ? &*semantic_policy : nullptr,
                                                         report_options,
                                                         semantic_warnings);
        }
        if (request.options.semantic_report_path.has_value()) {
            auto report_res = write_semantic_report_file(*request.options.semantic_report_path,
                                                         original_scene,
                                                         *scene_result,
                                                         semantic_policy ? &*semantic_policy : nullptr,
                                                         report_options,
                                                         semantic_warnings);
            if (!report_res) {
                return fail_with_report(report_res.error());
            }
        }
    }

    // Detect FLAC final output: FLAC does not carry float32 samples, so rendering
    // directly to FLAC would clip any > 0 dBFS content before loudness/peak
    // post-processing can compensate.  Use a float32 temp WAV as the render target
    // and encode to FLAC as the very last step, after all adjustments are applied.
    const auto final_path = std::filesystem::path(output_path);
    auto final_ext = final_path.extension().string();
    std::ranges::transform(final_ext, final_ext.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    const bool is_flac_final = (final_ext == ".flac");
    const bool is_opus_final = (final_ext == ".mka");
    const bool wants_apac_caf = request.options.apac_container == RenderOptions::ApacContainer::caf;
    if (wants_apac_caf && final_ext != ".caf") {
        const auto msg = fmt::format("--apac-container caf requires output path with .caf extension; got '{}'",
                                     final_ext.empty() ? std::string{"<none>"} : final_ext);
        return fail_with_report({ErrorCode::invalid_argument, msg, {}});
    }
    const bool is_apac_mpeg4_final = (final_ext == ".m4a" || final_ext == ".mp4") &&
                                     request.options.iamf_container != RenderOptions::IamfContainer::mp4 &&
                                     !wants_apac_caf;
    const bool is_apac_caf_final = wants_apac_caf && final_ext == ".caf";
    const bool is_apac_final = is_apac_mpeg4_final || is_apac_caf_final;
    const bool is_iamf_final = (final_ext == ".iamf");
    const bool is_iamf_mp4_final =
        (request.options.iamf_container == RenderOptions::IamfContainer::mp4) && audio::iamf_encoding_available();
    if (is_flac_final) {
        if (!flac_supports_layout(output_layout)) {
            const auto msg = fmt::format(
                "FLAC output supports only non-height layouts (binaural, 5.1, 7.1); layout '{}' is not supported",
                output_layout);
            return fail_with_report({ErrorCode::unsupported, msg, {}});
        }
        const auto channels = channel_count_for_layout(caps, output_layout);
        if (channels.has_value() && *channels > 8U) {
            const auto msg =
                fmt::format("FLAC supports 1-8 channels; layout '{}' renders {} channels", output_layout, *channels);
            return fail_with_report({ErrorCode::unsupported, msg, {}});
        }
    }
    if (is_iamf_final && !audio::iamf_encoding_available()) {
        constexpr auto msg =
            "IAMF output requires a build configured with MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools "
            "bridge";
        return fail_with_report({ErrorCode::unsupported, msg, {}});
    }
    if ((is_iamf_final || request.options.iamf_container == RenderOptions::IamfContainer::mp4) &&
        output_layout == "9.1.6") {
        constexpr auto msg =
            "IAMF 9.1.6 output is disabled because expanded/Base-Enhanced IAMF is not currently compatible enough "
            "for release output";
        return fail_with_report({ErrorCode::unsupported, msg, {}});
    }
    if (request.options.iamf_container == RenderOptions::IamfContainer::mp4) {
        if (final_ext != ".mp4") {
            const auto msg =
                fmt::format("--iamf-container mp4 requires output path with .mp4 extension; got '{}'", final_ext);
            return fail_with_report({ErrorCode::invalid_argument, msg, {}});
        }
        if (!audio::iamf_encoding_available()) {
            constexpr auto msg = "--iamf-container mp4 requires a build configured with MR_ADM_ENABLE_IAMF=ON";
            return fail_with_report({ErrorCode::unsupported, msg, {}});
        }
        const auto packager = audio::detect_iamf_mp4_packager();
        if (packager.kind == audio::IamfMp4PackagerKind::none) {
            constexpr auto msg = "--iamf-container mp4 requires mp4box (GPAC) or ffmpeg in PATH; "
                                 "install GPAC: https://gpac.io";
            return fail_with_report({ErrorCode::unsupported, msg, {}});
        }
        if (packager.kind == audio::IamfMp4PackagerKind::ffmpeg && packager.ffmpeg_major < 7) {
            logs.log(LogLevel::warning,
                     "engine",
                     fmt::format("ffmpeg {} detected; IAMF-in-MP4 ialb loudness box is unreliable "
                                 "below version 7.0 — consider installing mp4box (GPAC)",
                                 packager.ffmpeg_major));
        }
    }
    const bool is_lossy_final = (is_flac_final || is_opus_final || is_apac_final || is_iamf_final || is_iamf_mp4_final);
    const auto render_temp_path = is_lossy_final ? unique_render_temp_path(final_path) : std::filesystem::path{};
    auto render_temp_guard = is_lossy_final ? std::make_unique<TempFileGuard>(render_temp_path) : nullptr;
    const std::string render_path = is_lossy_final ? render_temp_path.string() : output_path;
    const auto output_temp_path = is_lossy_final ? unique_output_temp_path(final_path) : std::filesystem::path{};
    auto output_temp_guard = is_lossy_final ? std::make_unique<TempFileGuard>(output_temp_path) : nullptr;
    const std::string encoded_output_path = is_lossy_final ? output_temp_path.string() : output_path;
    OutputCleanupGuard output_guard{final_path};
    // For IAMF-in-MP4: intermediate .iamf temp alongside the render WAV temp.
    const auto iamf_temp_path = is_iamf_mp4_final
                                    ? render_temp_path.parent_path() / (render_temp_path.stem().string() + ".iamf")
                                    : std::filesystem::path{};
    auto iamf_temp_guard = is_iamf_mp4_final ? std::make_unique<TempFileGuard>(iamf_temp_path) : nullptr;

    // Build plan.
    emit_progress(progress, RenderStage::planning, RenderOperation::plan_render, 0.24, 0.0, "planning render");
    RenderPlan plan;
    plan.input_path = request.input_path.string();
    plan.output_path = render_path;
    plan.output_layout = output_layout;
    plan.sofa_path = request.options.sofa_path;
    plan.default_interp_ms = request.options.default_interp_ms;
    plan.object_smoothing_frames = request.options.object_smoothing_frames;
    plan.speaker_spread_mode = request.options.speaker_spread_mode;
    plan.binaural_spread_mode = request.options.binaural_spread_mode;
    plan.apple_spatial_preset = request.options.apple_spatial_preset;
    // Output time-range trim: prefer on-demand window rendering when the backend
    // supports it (seek + pre-roll → writes only the window, bit-identical to a full
    // render then trimmed, and skips the post-render trim pass below). Otherwise fall
    // back to a full render with the meter restricted to the kept window, then
    // trim_file_frames. The two are mutually exclusive on the plan.
    const bool window_render = need_trim && caps.supports_render_window;
    if (window_render) {
        plan.render_window = RenderWindow{trim_start_frame, trim_frame_count};
    } else if (need_trim) {
        plan.meter_window = MeterWindow{trim_start_frame, trim_frame_count};
    }
    plan.cancel_token = request.options.cancel_token;
    plan.scene = std::move(*scene_result);

    // Build (or reuse) the backend's immutable prepared state. A PreviewSession passes
    // a persistent cache slot so the gain matrices / HRTF are built once and reused
    // across windows; without a cache, prepare runs per render.
    std::shared_ptr<IPreparedRender> local_prepared;
    std::shared_ptr<IPreparedRender>& prepared_slot = (prepared_cache != nullptr) ? *prepared_cache : local_prepared;
    if (!prepared_slot) {
        emit_progress(
            progress, RenderStage::planning, RenderOperation::prepare_backend, 0.28, 0.0, "preparing backend");
        auto prep_res = renderer->prepare(plan, logs);
        if (!prep_res) {
            return fail_with_report(prep_res.error());
        }
        prepared_slot = std::move(*prep_res);
    }
    if (auto cancelled = fail_if_cancelled()) {
        return std::move(*cancelled);
    }

    // Render (inline measurement of loudness + True Peak over the meter window).
    ProgressRangeSink render_progress(
        progress, RenderStage::rendering, RenderOperation::render_audio, 0.30, 0.90, "rendering audio");
    auto render_res = renderer->render_window(*prepared_slot, plan, render_progress, logs);
    if (!render_res) {
        if (!is_lossy_final) {
            output_guard.arm();
            output_guard.remove_now();
        }
        const auto level = render_res.error().code == ErrorCode::cancelled ? LogLevel::info : LogLevel::error;
        return fail_with_report(render_res.error(), level);
    }
    const RenderMetrics& metrics = *render_res;
    if (!is_lossy_final) {
        output_guard.arm();
    }
    if (auto cancelled = fail_if_cancelled()) {
        return std::move(*cancelled);
    }

    if (metrics.measured_lufs) {
        logs.log(LogLevel::info, "engine", fmt::format("measured loudness: {:.1f} LUFS", *metrics.measured_lufs));
    }
    if (metrics.measured_peak_dbtp) {
        logs.log(LogLevel::info, "engine", fmt::format("measured true peak: {:.2f} dBTP", *metrics.measured_peak_dbtp));
    }

    // Apply the output time-range trim before gain/encode so every downstream step
    // operates on the trimmed PCM. The backend already measured loudness/True-Peak
    // over this same window (plan.meter_window), so `metrics`, the applied gain, and
    // the file metadata all describe the trimmed segment. Skipped when window_render
    // already produced a trimmed file (the backend wrote only the window).
    if (need_trim && !window_render) {
        ProgressRangeSink trim_progress(
            progress, RenderStage::post_processing, RenderOperation::trim_output, 0.90, 0.92, "trimming output");
        auto trim_res = audio::trim_file_frames(render_path,
                                                trim_start_frame,
                                                trim_frame_count,
                                                output_layout,
                                                plan.cancel_token,
                                                &trim_progress,
                                                RenderOperation::trim_output);
        if (!trim_res) {
            return fail_with_report(trim_res.error());
        }
    }
    if (auto cancelled = fail_if_cancelled()) {
        return std::move(*cancelled);
    }

    // Compute combined gain: loudness target first, optional peak makeup second,
    // peak ceiling third, then explicit final gain after the automatic stages.
    // Merging both into one apply_gain_to_file avoids a second read-write pass.
    double gain_db = 0.0;

    if (request.options.measure_loudness && metrics.measured_lufs.has_value()) {
        const auto target = static_cast<double>(request.options.loudness_target_lufs);
        const double delta = target - *metrics.measured_lufs;
        if (std::abs(delta) >= 0.1) {
            gain_db += delta;
            logs.log(
                LogLevel::info, "engine", fmt::format("loudness target {:.1f} LUFS → gain {:.2f} dB", target, delta));
        } else {
            logs.log(LogLevel::info, "engine", "integrated loudness within 0.1 LU of target — no adjustment");
        }
    }

    if (request.options.peak_normalize_to_limit && metrics.measured_peak_dbtp.has_value()) {
        const double peak_after = *metrics.measured_peak_dbtp + gain_db;
        const auto target_peak = static_cast<double>(request.options.peak_limit_dbtp);
        const double peak_makeup = target_peak - peak_after;
        if (peak_makeup > 0.1) {
            gain_db += peak_makeup;
            logs.log(LogLevel::info,
                     "engine",
                     fmt::format("true peak after loudness {:.2f} dBTP, target {:.1f} dBTP -> makeup {:.2f} dB",
                                 peak_after,
                                 target_peak,
                                 peak_makeup));
        } else {
            logs.log(LogLevel::info, "engine", "true peak already at/above normalization target; no makeup");
        }
    }

    if (request.options.peak_limit && metrics.measured_peak_dbtp.has_value()) {
        const double peak_after = *metrics.measured_peak_dbtp + gain_db;
        const auto target_peak = static_cast<double>(request.options.peak_limit_dbtp);
        const double peak_clamp = std::min(0.0, target_peak - peak_after);
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

    // Final gain: unconstrained user gain applied after all automatic staging.
    // Added after the peak clamp is computed, so it deliberately bypasses peak
    // limiting and may exceed the ceiling / 0 dBFS (integer outputs can clip).
    // Folded into gain_db so the applied gain, file metadata, and reported metrics
    // all include it.
    if (std::abs(request.options.final_gain_db) >= 1e-9) {
        gain_db += request.options.final_gain_db;
        logs.log(
            LogLevel::info,
            "engine",
            fmt::format("final gain {:+.2f} dB (unconstrained, bypasses peak limit)", request.options.final_gain_db));
    }

    if (std::abs(gain_db) >= 0.01) {
        const auto gain_linear = static_cast<float>(std::pow(10.0, gain_db / 20.0));
        logs.log(LogLevel::info, "engine", fmt::format("applying total gain {:.4f} ({:.2f} dB)", gain_linear, gain_db));
        ProgressRangeSink gain_progress(
            progress, RenderStage::post_processing, RenderOperation::apply_gain, 0.92, 0.94, "applying gain");
        auto gain_res = audio::apply_gain_to_file(
            render_path, gain_linear, output_layout, plan.cancel_token, &gain_progress, RenderOperation::apply_gain);
        if (!gain_res) {
            return fail_with_report(gain_res.error());
        }
    }
    if (auto cancelled = fail_if_cancelled()) {
        return std::move(*cancelled);
    }

    // Bit depth conversion: WAV output only.
    // CAF is always float32. For FLAC, the temp WAV stays float32 here so that
    // quantisation happens exactly once during the final FLAC encode step below.
    if (!is_lossy_final && request.options.output_bit_depth != OutputBitDepth::f32) {
        auto render_ext = std::filesystem::path(render_path).extension().string();
        std::ranges::transform(render_ext, render_ext.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        if (render_ext != ".caf") {
            const uint16_t depth = (request.options.output_bit_depth == OutputBitDepth::i16) ? 16U : 24U;
            logs.log(LogLevel::info, "engine", fmt::format("converting to {}-bit integer PCM", depth));
            ProgressRangeSink bitdepth_progress(progress,
                                                RenderStage::post_processing,
                                                RenderOperation::convert_bit_depth,
                                                0.94,
                                                0.955,
                                                "converting bit depth");
            auto conv_res = audio::downconvert_to_int(
                render_path, depth, plan.cancel_token, &bitdepth_progress, RenderOperation::convert_bit_depth);
            if (!conv_res) {
                return fail_with_report(conv_res.error());
            }
        }
    }
    if (auto cancelled = fail_if_cancelled()) {
        return std::move(*cancelled);
    }

    // FLAC final encode: the temp WAV is now fully post-processed (float32, gain
    // applied).  Encode to FLAC (24-bit) and remove the temp WAV regardless of
    // outcome to avoid leaving stale files on disk.
    if (is_flac_final) {
        logs.log(LogLevel::info, "engine", "encoding float32 render to FLAC (24-bit)");
        ProgressRangeSink flac_progress(
            progress, RenderStage::post_processing, RenderOperation::encode_flac, 0.955, 0.985, "encoding FLAC");
        auto flac_res = audio::convert_to_flac(render_path, encoded_output_path, plan.cancel_token, &flac_progress);
        render_temp_guard->remove_now();
        if (!flac_res) {
            return fail_with_report(flac_res.error());
        }
    }

    if (is_opus_final) {
        logs.log(LogLevel::info, "engine", "encoding float32 render to Opus MKA (VBR)");
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::encode_opus, 0.955, 0.0, "encoding Opus");
        auto opus_res = audio::convert_to_opus_mka(render_path,
                                                   encoded_output_path,
                                                   output_layout,
                                                   request.options.opus_bitrate_per_ch_kbps,
                                                   plan.cancel_token);
        render_temp_guard->remove_now();
        if (!opus_res) {
            return fail_with_report(opus_res.error());
        }
        emit_progress(progress, RenderStage::post_processing, RenderOperation::encode_opus, 0.985, 1.0, "Opus encoded");
    }

    if (is_apac_final) {
        logs.log(LogLevel::info, "engine", fmt::format("encoding float32 render to APAC ({})", final_ext));
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::encode_apac, 0.955, 0.0, "encoding APAC");
        const auto apac_container = is_apac_caf_final ? audio::ApacContainer::caf : audio::ApacContainer::mpeg4;
        auto apac_res = audio::convert_to_apac(render_path,
                                               encoded_output_path,
                                               output_layout,
                                               request.options.apac_bitrate_kbps,
                                               request.options.apac_drc_music,
                                               apac_container,
                                               plan.cancel_token);
        render_temp_guard->remove_now();
        if (!apac_res) {
            return fail_with_report(apac_res.error());
        }
        emit_progress(progress, RenderStage::post_processing, RenderOperation::encode_apac, 0.985, 1.0, "APAC encoded");
    }

    if (is_iamf_final) {
        logs.log(LogLevel::info, "engine", "encoding 32-bit PCM staging to IAMF (Opus, raw OBU stream)");
        const auto lufs =
            metrics.measured_lufs ? std::optional<double>(*metrics.measured_lufs + gain_db) : std::nullopt;
        const auto peak =
            metrics.measured_peak_dbtp ? std::optional<double>(*metrics.measured_peak_dbtp + gain_db) : std::nullopt;
        ProgressRangeSink bitdepth_progress(progress,
                                            RenderStage::post_processing,
                                            RenderOperation::convert_bit_depth,
                                            0.94,
                                            0.955,
                                            "converting bit depth");
        auto conv_res = audio::downconvert_to_int(
            render_path, 32U, plan.cancel_token, &bitdepth_progress, RenderOperation::convert_bit_depth);
        if (!conv_res) {
            return fail_with_report(conv_res.error());
        }
        if (auto cancelled = fail_if_cancelled()) {
            return std::move(*cancelled);
        }
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::encode_iamf, 0.955, 0.0, "encoding IAMF");
        auto iamf_res = audio::convert_to_iamf(render_path,
                                               encoded_output_path,
                                               output_layout,
                                               request.options.opus_bitrate_per_ch_kbps,
                                               lufs,
                                               peak,
                                               plan.cancel_token);
        render_temp_guard->remove_now();
        if (!iamf_res) {
            return fail_with_report(iamf_res.error());
        }
        emit_progress(progress, RenderStage::post_processing, RenderOperation::encode_iamf, 0.985, 1.0, "IAMF encoded");
    }

    if (is_iamf_mp4_final) {
        logs.log(LogLevel::info, "engine", "encoding 32-bit PCM staging to IAMF (Opus) and packaging to MP4");
        const auto lufs =
            metrics.measured_lufs ? std::optional<double>(*metrics.measured_lufs + gain_db) : std::nullopt;
        const auto peak =
            metrics.measured_peak_dbtp ? std::optional<double>(*metrics.measured_peak_dbtp + gain_db) : std::nullopt;
        ProgressRangeSink bitdepth_progress(progress,
                                            RenderStage::post_processing,
                                            RenderOperation::convert_bit_depth,
                                            0.94,
                                            0.955,
                                            "converting bit depth");
        auto conv_res = audio::downconvert_to_int(
            render_path, 32U, plan.cancel_token, &bitdepth_progress, RenderOperation::convert_bit_depth);
        if (!conv_res) {
            return fail_with_report(conv_res.error());
        }
        if (auto cancelled = fail_if_cancelled()) {
            return std::move(*cancelled);
        }
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::encode_iamf, 0.955, 0.0, "encoding IAMF");
        auto iamf_res = audio::convert_to_iamf(render_path,
                                               iamf_temp_path.string(),
                                               output_layout,
                                               request.options.opus_bitrate_per_ch_kbps,
                                               lufs,
                                               peak,
                                               plan.cancel_token);
        render_temp_guard->remove_now();
        if (!iamf_res) {
            return fail_with_report(iamf_res.error());
        }
        emit_progress(progress, RenderStage::post_processing, RenderOperation::encode_iamf, 0.975, 1.0, "IAMF encoded");
        emit_progress(progress,
                      RenderStage::post_processing,
                      RenderOperation::package_iamf_mp4,
                      0.975,
                      0.0,
                      "packaging IAMF MP4");
        auto pkg_res = audio::package_iamf_to_mp4(iamf_temp_path.string(), encoded_output_path, plan.cancel_token);
        iamf_temp_guard->remove_now();
        if (!pkg_res) {
            return fail_with_report(pkg_res.error());
        }
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::package_iamf_mp4, 0.985, 1.0, "IAMF MP4 packaged");
    }

    if (auto cancelled = fail_if_cancelled()) {
        return std::move(*cancelled);
    }
    if (is_lossy_final) {
        auto replace_res = replace_output_file(output_temp_path, final_path);
        if (!replace_res) {
            return fail_with_report(replace_res.error());
        }
        output_temp_guard->dismiss();
    }

    // Write format-specific metadata (non-fatal on failure).
    {
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::write_metadata, 0.99, 0.0, "writing metadata");
        const std::time_t now = std::time(nullptr);
        const std::tm tm_utc = utc_time(now);

        audio::MetadataFields meta_fields;
        meta_fields.encoder = "MacinRender ADM Core Alpha";
        meta_fields.date_utc = fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
                                           tm_utc.tm_year + 1900,
                                           tm_utc.tm_mon + 1,
                                           tm_utc.tm_mday,
                                           tm_utc.tm_hour,
                                           tm_utc.tm_min,
                                           tm_utc.tm_sec);
        meta_fields.renderer = caps.backend_name;
        meta_fields.output_layout = output_layout;
        // bext / info fields must reflect the actual file after gain adjustment.
        meta_fields.lufs =
            metrics.measured_lufs ? std::optional<double>(*metrics.measured_lufs + gain_db) : std::nullopt;
        meta_fields.peak_dbtp =
            metrics.measured_peak_dbtp ? std::optional<double>(*metrics.measured_peak_dbtp + gain_db) : std::nullopt;

        auto meta_res = audio::write_file_metadata(output_path, meta_fields);
        if (!meta_res) {
            logs.log(LogLevel::warning, "engine", "metadata write failed: " + meta_res.error().message);
        }
        emit_progress(
            progress, RenderStage::post_processing, RenderOperation::write_metadata, 0.995, 1.0, "metadata written");
    }
    output_guard.commit();

    // Return the post-processing effective values (gain-adjusted), matching what was
    // written to the file's metadata. Raw renderer measurements live in `metrics`.
    const RenderMetrics final_metrics{
        metrics.measured_lufs ? std::optional<double>(*metrics.measured_lufs + gain_db) : std::nullopt,
        metrics.measured_peak_dbtp ? std::optional<double>(*metrics.measured_peak_dbtp + gain_db) : std::nullopt,
    };
    emit_progress(progress, RenderStage::finished, RenderOperation::finish, 1.0, 1.0, "done");
    return {{ErrorCode::ok, "", {}},
            std::filesystem::path{output_path},
            final_metrics,
            {{LogLevel::info, "render completed"}},
            std::move(semantic_report_json)};
}

Result<SceneProbe> RenderService::probe(const std::string& input_path) const {
    auto scene_result = io::import_scene(input_path);
    if (!scene_result) {
        return tl::unexpected(scene_result.error());
    }
    const auto& scene = *scene_result;
    SceneProbe probe;
    probe.sample_rate = scene.info.sample_rate;
    probe.num_channels = scene.info.num_channels;
    probe.num_frames = scene.info.num_frames;
    probe.programme_count = static_cast<uint32_t>(scene.programmes.size());
    probe.object_count = static_cast<uint32_t>(scene.objects.size());
    return probe;
}

Result<std::string> RenderService::inspect_json(const std::string& input_path) const {
    auto scene_result = io::import_scene(input_path);
    if (!scene_result) {
        return tl::unexpected(scene_result.error());
    }
    return engine::scene_to_json(*scene_result);
}

std::string RenderService::capabilities_json() const {
    return engine::capabilities_to_json();
}

std::vector<OutputLayoutRow> RenderService::output_layouts() const {
    return engine::build_output_layouts();
}

std::string RenderService::layouts_json() const {
    return engine::layouts_to_json();
}

OutputFormats RenderService::output_formats() const {
    return engine::build_output_formats();
}

std::string RenderService::output_formats_json() const {
    return engine::output_formats_to_json();
}

Result<std::string> RenderService::axml(const std::string& input_path) const {
    return io::get_axml(input_path);
}

Result<std::string> RenderService::policy_template_json(const std::string& input_path) const {
    auto scene_result = io::import_scene(input_path);
    if (!scene_result) {
        return tl::unexpected(scene_result.error());
    }
    return build_semantic_policy_template(*scene_result);
}

Result<AdmScene> RenderService::prepare_preview_scene(const std::filesystem::path& input_path,
                                                      const RenderOptions& options,
                                                      LogSink& logs) const {
    auto scene = io::import_scene(input_path.string());
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    for (const auto& w : scene->import_warnings) {
        logs.log(LogLevel::warning, "importer", w);
    }
    std::vector<std::string> warnings;
    auto policy_res = resolve_and_apply_policy(*scene, options, warnings, logs);
    if (!policy_res) {
        return tl::unexpected(policy_res.error());
    }
    return scene;
}

// ── PreviewSession ──────────────────────────────────────────────────────────────

PreviewSession::PreviewSession(std::filesystem::path input, RenderOptions options, AdmScene scene)
    : input_(std::move(input)), options_(std::move(options)), scene_(std::move(scene)) {}

Result<PreviewSession> PreviewSession::create(std::filesystem::path input_path, RenderOptions options, LogSink& logs) {
    RenderService service;
    auto imported = service.prepare_preview_scene(input_path, options, logs);
    if (!imported) {
        return tl::unexpected(imported.error());
    }
    return PreviewSession{std::move(input_path), std::move(options), std::move(*imported)};
}

RenderResult PreviewSession::render_window(std::optional<double> start_sec,
                                           std::optional<double> end_sec,
                                           std::optional<std::filesystem::path> output_path,
                                           ProgressSink& progress,
                                           LogSink& logs) const {
    RenderRequest request;
    request.input_path = input_;
    request.output_path = std::move(output_path);
    request.options = options_;
    request.options.render_start_sec = start_sec.value_or(0.0);
    request.options.render_end_sec = end_sec;
    return service_.render(request, progress, logs, &scene_, &prepared_);
}

} // namespace mradm
