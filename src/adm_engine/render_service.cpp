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

[[nodiscard]] std::filesystem::path unique_render_temp_path(const std::filesystem::path& final_path) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;

    const auto parent = final_path.parent_path();
    const auto stem = final_path.stem().string();
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto candidate = parent / fmt::format("{}.render_tmp.{:016x}.wav", stem, dist(rng));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return parent / fmt::format("{}.render_tmp.{:016x}.wav", stem, dist(rng));
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
    }
    return "unknown";
}

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

  private:
    std::filesystem::path path_;
    bool active_{true};
};

} // anonymous namespace

RenderService::RenderService() = default;

// RenderService intentionally owns the full orchestration pipeline; splitting it
// mechanically would hide the ordering constraints between render, post-process,
// encode, and metadata.
// NOLINTNEXTLINE(readability-function-size)
RenderResult RenderService::render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const {
    progress.on_progress({RenderStage::validating, 0.0, "validating request"});

    if (request.input_path.empty()) {
        return {{ErrorCode::invalid_argument, "input path is required", {}}, std::nullopt, std::nullopt, {}};
    }

    logs.log(LogLevel::info, "engine", fmt::format("render request: {}", request.input_path.string()));

    if (request.options.peak_normalize_to_limit && !request.options.peak_limit) {
        const auto msg = std::string{"peak normalization to limit requires peak limiting to be enabled"};
        return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }

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

    const auto requested_layout = normalize_output_layout(
        request.options.output_layout.empty() ? std::string{"0+2+0"} : request.options.output_layout);
    const bool requests_speaker_stereo = (requested_layout == "0+2+0");

    // Select backend. Speaker stereo rendering is intentionally not exposed:
    // the current 2ch speaker projection is not a downmix and can be badly
    // misleading for ADM content. Automatic 2ch output therefore means binaural.
    auto sel = request.options.renderer;
    if (sel == RendererSelection::automatic && (requests_speaker_stereo || requested_layout == "binaural")) {
        sel = RendererSelection::binaural;
    }
    if ((sel == RendererSelection::ear || sel == RendererSelection::saf) && requests_speaker_stereo &&
        !request.options.internal_allow_speaker_stereo) {
        const auto msg =
            std::string{"speaker stereo rendering is disabled; use --renderer binaural for 2ch ADM output"};
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }

    std::unique_ptr<IRenderer> renderer;
    if (sel == RendererSelection::ear || sel == RendererSelection::automatic) {
        renderer = create_ear_renderer();
    } else if (sel == RendererSelection::saf) {
        renderer = create_vbap_renderer();
    } else if (sel == RendererSelection::hoa) {
        renderer = create_hoa_renderer();
    } else if (sel == RendererSelection::binaural) {
        renderer = create_binaural_renderer();
    } else {
        const auto msg = fmt::format("renderer '{}' is not available in this build", static_cast<int>(sel));
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }

    const auto caps = renderer->capabilities();
    logs.log(LogLevel::info, "engine", fmt::format("backend: {} {}", caps.backend_name, caps.backend_version));

    std::optional<SemanticPolicy> semantic_policy;
    std::vector<std::string> semantic_warnings;
    const bool needs_semantic_report = request.options.semantic_report_path.has_value();
    const AdmScene original_scene = needs_semantic_report ? *scene_result : AdmScene{};

    auto output_layout = requested_layout;
    if (sel == RendererSelection::binaural) {
        if (requested_layout != "0+2+0" && requested_layout != "binaural") {
            logs.log(LogLevel::warning,
                     "engine",
                     fmt::format("binaural renderer always writes 2ch HRTF output; ignoring requested layout '{}'",
                                 requested_layout));
        }
        output_layout = "binaural";
    }

    if (request.options.semantic_policy_path.has_value()) {
        auto policy_res = load_semantic_policy_file(*request.options.semantic_policy_path);
        if (!policy_res) {
            return {policy_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, policy_res.error().message}}};
        }
        semantic_policy = std::move(*policy_res);
        auto apply_res =
            apply_semantic_policy(*scene_result, *semantic_policy, scene_result->info.sample_rate, &semantic_warnings);
        if (!apply_res) {
            return {apply_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, apply_res.error().message}}};
        }
        for (const auto& warning : semantic_warnings) {
            logs.log(LogLevel::warning, "semantic-policy", warning);
        }
    }

    if (needs_semantic_report) {
        const SemanticPolicyReportOptions report_options{
            .renderer = renderer_name(sel),
            .policy_path =
                request.options.semantic_policy_path ? request.options.semantic_policy_path->string() : std::string{},
            .capabilities = caps,
        };
        auto report_res = write_semantic_report_file(*request.options.semantic_report_path,
                                                     original_scene,
                                                     *scene_result,
                                                     semantic_policy ? &*semantic_policy : nullptr,
                                                     report_options,
                                                     semantic_warnings);
        if (!report_res) {
            return {report_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, report_res.error().message}}};
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
    const bool is_apac_final = (final_ext == ".m4a" || final_ext == ".mp4") &&
                               request.options.iamf_container != RenderOptions::IamfContainer::mp4;
    const bool is_iamf_final = (final_ext == ".iamf");
    const bool is_iamf_mp4_final = (request.options.iamf_container == RenderOptions::IamfContainer::mp4) &&
                                   audio::iamf_encoding_available();
    if (is_flac_final) {
        if (!flac_supports_layout(output_layout)) {
            const auto msg = fmt::format(
                "FLAC output supports only non-height layouts (binaural, 5.1, 7.1); layout '{}' is not supported",
                output_layout);
            return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
        }
        const auto channels = channel_count_for_layout(caps, output_layout);
        if (channels.has_value() && *channels > 8U) {
            const auto msg =
                fmt::format("FLAC supports 1-8 channels; layout '{}' renders {} channels", output_layout, *channels);
            return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
        }
    }
    if (is_iamf_final && !audio::iamf_encoding_available()) {
        constexpr auto msg =
            "IAMF output requires a build configured with MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools "
            "bridge";
        return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
    }
    if (request.options.iamf_container == RenderOptions::IamfContainer::mp4) {
        if (final_ext != ".mp4") {
            const auto msg = fmt::format(
                "--iamf-container mp4 requires output path with .mp4 extension; got '{}'", final_ext);
            return {{ErrorCode::invalid_argument, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
        }
        if (!audio::iamf_encoding_available()) {
            constexpr auto msg =
                "--iamf-container mp4 requires a build configured with MR_ADM_ENABLE_IAMF=ON";
            return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
        }
        const auto packager = audio::detect_iamf_mp4_packager();
        if (packager.kind == audio::IamfMp4PackagerKind::none) {
            constexpr auto msg =
                "--iamf-container mp4 requires mp4box (GPAC) or ffmpeg in PATH; "
                "install GPAC: https://gpac.io";
            return {{ErrorCode::unsupported, msg, {}}, std::nullopt, std::nullopt, {{LogLevel::error, msg}}};
        }
        if (packager.kind == audio::IamfMp4PackagerKind::ffmpeg && packager.ffmpeg_major < 7) {
            logs.log(LogLevel::warning, "engine",
                     fmt::format("ffmpeg {} detected; IAMF-in-MP4 ialb loudness box is unreliable "
                                 "below version 7.0 — consider installing mp4box (GPAC)",
                                 packager.ffmpeg_major));
        }
    }
    const bool is_lossy_final = (is_flac_final || is_opus_final || is_apac_final || is_iamf_final || is_iamf_mp4_final);
    const auto render_temp_path = is_lossy_final ? unique_render_temp_path(final_path) : std::filesystem::path{};
    auto render_temp_guard = is_lossy_final ? std::make_unique<TempFileGuard>(render_temp_path) : nullptr;
    const std::string render_path = is_lossy_final ? render_temp_path.string() : output_path;
    // For IAMF-in-MP4: intermediate .iamf temp alongside the render WAV temp.
    const auto iamf_temp_path = is_iamf_mp4_final
                                    ? render_temp_path.parent_path() /
                                          (render_temp_path.stem().string() + ".iamf")
                                    : std::filesystem::path{};
    auto iamf_temp_guard = is_iamf_mp4_final ? std::make_unique<TempFileGuard>(iamf_temp_path) : nullptr;

    // Build plan.
    RenderPlan plan;
    plan.input_path = request.input_path.string();
    plan.output_path = render_path;
    plan.output_layout = output_layout;
    plan.sofa_path = request.options.sofa_path;
    plan.default_interp_ms = request.options.default_interp_ms;
    plan.object_smoothing_frames = request.options.object_smoothing_frames;
    plan.speaker_spread_mode = request.options.speaker_spread_mode;
    plan.binaural_spread_mode = request.options.binaural_spread_mode;
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
        logs.log(LogLevel::info, "engine", fmt::format("measured true peak: {:.2f} dBTP", *metrics.measured_peak_dbtp));
    }

    // Compute combined gain: loudness target first, optional peak makeup second,
    // then peak ceiling as the final red line.
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

    if (std::abs(gain_db) >= 0.01) {
        const auto gain_linear = static_cast<float>(std::pow(10.0, gain_db / 20.0));
        logs.log(LogLevel::info, "engine", fmt::format("applying total gain {:.4f} ({:.2f} dB)", gain_linear, gain_db));
        auto gain_res = audio::apply_gain_to_file(render_path, gain_linear, output_layout);
        if (!gain_res) {
            return {gain_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, gain_res.error().message}}};
        }
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
            auto conv_res = audio::downconvert_to_int(render_path, depth);
            if (!conv_res) {
                return {conv_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, conv_res.error().message}}};
            }
        }
    }

    // FLAC final encode: the temp WAV is now fully post-processed (float32, gain
    // applied).  Encode to FLAC (24-bit) and remove the temp WAV regardless of
    // outcome to avoid leaving stale files on disk.
    if (is_flac_final) {
        logs.log(LogLevel::info, "engine", "encoding float32 render to FLAC (24-bit)");
        auto flac_res = audio::convert_to_flac(render_path, output_path);
        render_temp_guard->remove_now();
        if (!flac_res) {
            return {flac_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, flac_res.error().message}}};
        }
    }

    if (is_opus_final) {
        logs.log(LogLevel::info, "engine", "encoding float32 render to Opus MKA (VBR)");
        auto opus_res = audio::convert_to_opus_mka(
            render_path, output_path, output_layout, request.options.opus_bitrate_per_ch_kbps);
        render_temp_guard->remove_now();
        if (!opus_res) {
            return {opus_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, opus_res.error().message}}};
        }
    }

    if (is_apac_final) {
        logs.log(LogLevel::info, "engine", fmt::format("encoding float32 render to APAC ({})", final_ext));
        auto apac_res = audio::convert_to_apac(
            render_path, output_path, output_layout, request.options.apac_bitrate_kbps, request.options.apac_drc_music);
        render_temp_guard->remove_now();
        if (!apac_res) {
            return {apac_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, apac_res.error().message}}};
        }
    }

    if (is_iamf_final) {
        logs.log(LogLevel::info, "engine", "encoding float32 render to IAMF (Opus, raw OBU stream)");
        const auto lufs =
            metrics.measured_lufs ? std::optional<double>(*metrics.measured_lufs + gain_db) : std::nullopt;
        const auto peak =
            metrics.measured_peak_dbtp ? std::optional<double>(*metrics.measured_peak_dbtp + gain_db) : std::nullopt;
        auto iamf_res = audio::convert_to_iamf(
            render_path, output_path, output_layout, request.options.opus_bitrate_per_ch_kbps, lufs, peak);
        render_temp_guard->remove_now();
        if (!iamf_res) {
            return {iamf_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, iamf_res.error().message}}};
        }
    }

    if (is_iamf_mp4_final) {
        logs.log(LogLevel::info, "engine", "encoding float32 render to IAMF (Opus) and packaging to MP4");
        const auto lufs =
            metrics.measured_lufs ? std::optional<double>(*metrics.measured_lufs + gain_db) : std::nullopt;
        const auto peak =
            metrics.measured_peak_dbtp ? std::optional<double>(*metrics.measured_peak_dbtp + gain_db) : std::nullopt;
        auto iamf_res = audio::convert_to_iamf(render_path, iamf_temp_path.string(), output_layout,
                                               request.options.opus_bitrate_per_ch_kbps, lufs, peak);
        render_temp_guard->remove_now();
        if (!iamf_res) {
            return {iamf_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, iamf_res.error().message}}};
        }
        auto pkg_res = audio::package_iamf_to_mp4(iamf_temp_path.string(), output_path);
        iamf_temp_guard->remove_now();
        if (!pkg_res) {
            return {pkg_res.error(), std::nullopt, std::nullopt, {{LogLevel::error, pkg_res.error().message}}};
        }
    }

    // Write format-specific metadata (non-fatal on failure).
    {
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
    }

    return {
        {ErrorCode::ok, "", {}}, std::filesystem::path{output_path}, metrics, {{LogLevel::info, "render completed"}}};
}

} // namespace mradm
