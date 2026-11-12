#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/options.h"
#include "adm/render.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

#include "../adm_audio/island_ffi.h"
#include "../adm_core/direct_matrix_records.h"
#include "../adm_io/scene_import_internal.h"
#include "../adm_render_room/room_internal.h"
#include "capability_records.h"
#include "catalog.h"
#include "engine_records.h"
#include "engine_result.h"
#include "request_record.h"
#include "scene_handle.h"

namespace mradm {

namespace {

using engine::apple_renderer;
using engine::engine_path;
using engine::RequestTexts;
using engine::utf8;
using render_common::engine_text;

// The native spelling of a UTF-8 path from the engine (the islands open native paths).
[[nodiscard]] std::string native(MradmAdmText text) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(text.data), text.len);
    return std::filesystem::path(bytes).string();
}

[[nodiscard]] Error error_of(int32_t code, MradmAdmText message, MradmAdmText context) {
    const auto kind = code >= 0 && code <= 6 ? static_cast<ErrorCode>(code) : ErrorCode::internal_error;
    return {kind, engine_text(message), engine_text(context)};
}


using RenderHandle = std::unique_ptr<MradmEngineRender, decltype(&mradm_engine_render_destroy)>;

[[nodiscard]] ProgressEvent progress_of(const MradmEngineProgress& p) {
    return {static_cast<RenderStage>(p.stage),
            static_cast<RenderOperation>(p.operation),
            p.fraction,
            p.stage_fraction,
            p.current,
            p.total,
            std::string_view{reinterpret_cast<const char*>(p.message.data), p.message.len}};
}

// An island's progress mapped into the render's range by the engine.
class MappedProgress final : public ProgressSink {
  public:
    MappedProgress(const MradmEngineRender* render, ProgressSink& downstream)
        : render_(render), downstream_(&downstream) {}
    void on_progress(const ProgressEvent& event) override {
        const MradmEngineProgress raw{static_cast<uint32_t>(event.stage),
                                      static_cast<uint32_t>(event.operation),
                                      event.fraction,
                                      event.stage_fraction,
                                      event.current_frame,
                                      event.total_frames,
                                      engine_text(event.message)};
        MradmEngineProgress mapped{};
        if (mradm_engine_render_map_progress(render_, &raw, &mapped) == 0) {
            downstream_->on_progress(progress_of(mapped));
        }
    }

  private:
    const MradmEngineRender* render_;
    ProgressSink* downstream_;
};

// The caller's stop token and progress for the islands the engine runs (mradm_audio_island).
struct IslandDriver {
    const std::stop_token* token{nullptr};
    ProgressSink* progress{nullptr};
    std::exception_ptr failure = nullptr;
    std::atomic<bool> failed{false};
};

// cppcheck-suppress constParameterCallback ; matches the private C callback signature.
uint32_t island_cancelled(void* user) noexcept {
    const auto& driver = *static_cast<const IslandDriver*>(user);
    return driver.failed.load() || driver.token->stop_requested() ? 1U : 0U;
}

void island_progress(void* user, const MradmEngineProgress* event) noexcept {
    auto& driver = *static_cast<IslandDriver*>(user);
    if (driver.failed.load()) {
        return;
    }
    try {
        driver.progress->on_progress(progress_of(*event));
    } catch (...) {
        // Keep exceptions on the C++ side of the Rust callback boundary.
        driver.failure = std::current_exception();
        driver.failed.store(true);
    }
}

// The Apple backend island of a render (the engine runs the audio islands). The reply's texts
// live until the engine resumes.
class Islands {
  public:
    Islands(const MradmEngineRender* render,
            const RenderRequest& request,
            ProgressSink& progress,
            LogSink& logs,
            std::shared_ptr<IPreparedRender>& slot)
        : render_(render), request_(&request), progress_(&progress), logs_(&logs), slot_(&slot) {}

    const MradmEngineIslandReply& run(const MradmEngineRenderEvent& e) {
        message_.clear();
        context_.clear();
        reply_ = {};
        reply_.ffmpeg_major = -1;
        MappedProgress mapped(render_, *progress_);
        if (e.island == 2U || e.island == 3U) {
            apple(e, native(e.dst.utf8), mapped);
        } else {
            fail({ErrorCode::internal_error, "unknown render island", {}});
        }
        return reply_;
    }

  private:
    void fail(const Error& error) {
        message_ = error.message;
        context_ = error.context;
        reply_.code = static_cast<int32_t>(error.code);
        reply_.message = engine_text(message_);
        reply_.context = engine_text(context_);
    }

    // The Apple backend prepares and renders its own windows over the engine's scene and plan.
    void apple(const MradmEngineRenderEvent& e, const std::string& output, ProgressSink& mapped) {
        const auto& o = request_->options;
        auto scene = io::RenderScene::adopt(mradm_engine_render_scene(render_), request_->input_path.string());
        if (!scene) {
            fail(scene.error());
            return;
        }
        RenderPlan plan;
        plan.input_path = request_->input_path.string();
        plan.output_path = output;
        plan.output_layout = engine_text(e.layout);
        plan.sofa_path = o.sofa_path;
        plan.room = o.room;
        plan.screens = o.screens;
        plan.default_interp_ms = o.default_interp_ms;
        plan.object_smoothing_frames = o.object_smoothing_frames;
        plan.speaker_geometry = o.speaker_geometry;
        plan.direct_speakers_routing_mode = o.direct_speakers_routing_mode;
        if (e.has_matrix != 0U) {
            plan.direct_speakers_matrix =
                std::make_shared<const DirectSpeakersMatrix>(detail::matrix_from_view(e.matrix));
        }
        plan.speaker_spread_mode = o.speaker_spread_mode;
        plan.triple_balance_mode = o.triple_balance_mode;
        plan.binaural_spread_mode = o.binaural_spread_mode;
        plan.lfe_routing_mode = o.lfe_routing_mode;
        plan.apple_spatial_preset = o.apple_spatial_preset;
        plan.apple_speaker_rendering_flags = o.apple_speaker_rendering_flags;
        plan.listener_orientation = o.listener_orientation;
        if (e.has_window != 0U) {
            plan.render_window = RenderWindow{e.window_start, e.window_count};
        }
        if (e.has_meter_window != 0U) {
            plan.meter_window = MeterWindow{e.meter_start, e.meter_count};
        }
        plan.cancel_token = o.cancel_token;
        plan.scene_handle = scene->handle();
        auto renderer = apple_renderer();
        if (!renderer) {
            fail(renderer.error());
            return;
        }
        if (e.island == 2U) {
            auto prepared = (*renderer)->prepare(plan, *logs_);
            if (!prepared) {
                fail(prepared.error());
                return;
            }
            *slot_ = std::move(*prepared);
            return;
        }
        auto metrics = (*renderer)->render_window(**slot_, plan, mapped, *logs_);
        if (!metrics) {
            fail(metrics.error());
            return;
        }
        reply_.has_lufs = metrics->measured_lufs ? 1U : 0U;
        reply_.lufs = metrics->measured_lufs.value_or(0.0);
        reply_.has_peak = metrics->measured_peak_dbtp ? 1U : 0U;
        reply_.peak_dbtp = metrics->measured_peak_dbtp.value_or(0.0);
    }

    const MradmEngineRender* render_;
    const RenderRequest* request_;
    ProgressSink* progress_;
    LogSink* logs_;
    std::shared_ptr<IPreparedRender>* slot_;
    MradmEngineIslandReply reply_{};
    std::string message_;
    std::string context_;
};

[[nodiscard]] RenderResult result_of(MradmEngineRender* render) {
    MradmEngineRenderResult r{};
    if (mradm_engine_render_result(render, &r) != 0) {
        return {{ErrorCode::internal_error, "render result is unavailable", {}}, std::nullopt, std::nullopt, {}};
    }
    RenderResult result{error_of(r.code, r.message, r.context), std::nullopt, std::nullopt, {}};
    if (r.has_output != 0U) {
        const std::u8string bytes(reinterpret_cast<const char8_t*>(r.output_path.data), r.output_path.len);
        result.output_path = std::filesystem::path(bytes);
    }
    if (r.has_metrics != 0U) {
        RenderMetrics metrics;
        if (r.has_lufs != 0U) {
            metrics.measured_lufs = r.lufs;
        }
        if (r.has_peak != 0U) {
            metrics.measured_peak_dbtp = r.peak_dbtp;
        }
        result.metrics = metrics;
    }
    for (const auto& line : std::span{r.logs, r.logs_len}) {
        result.diagnostics.push_back({static_cast<LogLevel>(line.level), engine_text(line.message)});
    }
    if (r.has_report != 0U) {
        result.semantic_report_json = engine_text(r.report);
    }
    return result;
}

[[nodiscard]] Result<void> log_import_warnings(const io::RenderScene& scene, LogSink& logs) {
    auto summary = scene.summary();
    if (!summary) {
        return tl::unexpected{summary.error()};
    }
    for (const auto& warning : summary->warnings) {
        logs.log(LogLevel::warning, "importer", warning);
    }
    return {};
}

// The policy of a preview or export import, in Rust (mradm-engine `render`).
[[nodiscard]] Result<void>
apply_policy(io::RenderScene& scene, const std::string& input, const RenderOptions& options, LogSink& logs) {
    const auto json = options.semantic_policy_json;
    const auto path = options.semantic_policy_path ? utf8(*options.semantic_policy_path) : std::string{};
    const auto display = utf8(input);
    const render_common::EngineOutcome outcome([&](MradmEngineResult** out, MradmEngineStatus* status) {
        return mradm_engine_apply_policy(scene.handle()->raw,
                                         json ? 1U : 0U,
                                         engine_text(json ? std::string_view{*json} : std::string_view{}),
                                         options.semantic_policy_path ? 1U : 0U,
                                         engine_path(path),
                                         engine_text(display),
                                         out,
                                         status);
    });
    outcome.emit(logs, "semantic-policy");
    if (!outcome.ok()) {
        return tl::unexpected{outcome.error()};
    }
    return {};
}

} // anonymous namespace

RenderService::RenderService() = default;

// The offline render is a Rust step machine (mradm-engine `render`): this
// loop forwards its progress and logs, passes the stop token before every step and executes the
// islands it requests.
RenderResult RenderService::render(const RenderRequest& request,
                                   ProgressSink& progress,
                                   LogSink& logs,
                                   const PreparedScene* preimported_scene,
                                   std::shared_ptr<IPreparedRender>* prepared_cache) const {
    const RequestTexts texts(request);
    const auto record = texts.record(request);
    const engine::RenderPlatform platform;
    std::shared_ptr<IPreparedRender> local;
    auto& slot = prepared_cache != nullptr ? *prepared_cache : local;
    const auto* cached = dynamic_cast<const render_common::EnginePrepared*>(slot.get());
    MradmEngineRender* raw = nullptr;
    const auto code = mradm_engine_render_begin(&record,
                                                platform.get(),
                                                preimported_scene != nullptr ? preimported_scene->handle->raw : nullptr,
                                                cached != nullptr ? cached->handle.get() : nullptr,
                                                slot != nullptr && cached == nullptr ? 1U : 0U,
                                                &raw);
    if (code != 0) {
        return {
            error_of(code, engine_text("invalid render request (UTF-8 paths without NUL required)"), {}), {}, {}, {}};
    }
    const RenderHandle render(raw, &mradm_engine_render_destroy);
    IslandDriver driver{&request.options.cancel_token, &progress};
    const MradmIslandRunner runner{&mradm_audio_island, {&driver, &island_cancelled, &island_progress}};
    if (mradm_engine_render_set_islands(render.get(), &runner) != 0) {
        return {{ErrorCode::internal_error, "render islands unavailable", {}}, std::nullopt, std::nullopt, {}};
    }
    Islands islands(render.get(), request, progress, logs, slot);
    for (;;) {
        MradmEngineRenderEvent event{};
        const auto status =
            mradm_engine_render_next(render.get(), request.options.cancel_token.stop_requested() ? 1U : 0U, &event);
        if (driver.failure != nullptr) {
            std::rethrow_exception(driver.failure);
        }
        if (status != 0) {
            return {{ErrorCode::internal_error, "render step failed", {}}, std::nullopt, std::nullopt, {}};
        }
        if (event.kind == 1U) {
            progress.on_progress(progress_of(event.progress));
        } else if (event.kind == 2U) {
            logs.log(static_cast<LogLevel>(event.level), engine_text(event.module), engine_text(event.message));
        } else if (event.kind == 3U) {
            const auto& reply = islands.run(event);
            if (mradm_engine_render_resume(render.get(), &reply) != 0) {
                return {{ErrorCode::internal_error, "render island reply failed", {}}, std::nullopt, std::nullopt, {}};
            }
        } else if (event.kind == 4U) {
            break;
        }
    }
    if (prepared_cache != nullptr) {
        if (auto* made = mradm_engine_render_take_prepared(render.get()); made != nullptr) {
            auto prepared = std::make_shared<render_common::EnginePrepared>();
            prepared->handle.reset(made);
            slot = std::move(prepared);
        }
    }
    return result_of(render.get());
}

Result<SceneProbe> RenderService::probe(const std::string& input_path) const {
    const io::SceneImportOptions no_options{};
    auto scene_result = io::RenderScene::import(input_path, &no_options);
    if (!scene_result) {
        // Probe remains useful before the caller has chosen an explicit mapping:
        // return raw WAVE audio facts even when its channel mask is unknown.
        auto wave = audio::FloatWavReader::open(input_path);
        if (!wave || wave->channels() == 0U || wave->channels() > std::numeric_limits<uint16_t>::max()) {
            return tl::unexpected(scene_result.error());
        }
        SceneProbe raw;
        raw.sample_rate = wave->sample_rate();
        raw.num_channels = static_cast<uint16_t>(wave->channels());
        raw.num_frames = wave->frame_count();
        return raw;
    }
    auto summary = scene_result->summary();
    if (!summary) {
        return tl::unexpected(summary.error());
    }
    SceneProbe probe;
    probe.sample_rate = summary->sample_rate;
    probe.num_channels = summary->channels;
    probe.num_frames = summary->frames;
    probe.programme_count = summary->programmes;
    probe.object_count = summary->objects;
    return probe;
}

Result<std::string> RenderService::inspect_json(const std::string& input_path) const {
    const io::SceneImportOptions no_options{};
    auto scene = io::RenderScene::import(input_path, &no_options);
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    return scene->inspect_json();
}

Result<std::string> RenderService::inspect_text(const std::string& input_path) const {
    const io::SceneImportOptions no_options{};
    auto scene = io::RenderScene::import(input_path, &no_options);
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    auto text = scene->inspect_text();
    if (!text) {
        return tl::unexpected(text.error());
    }
    return "File: " + input_path + "\n" + *text;
}

std::string RenderService::capabilities_json() const {
    return engine::document(engine::Document::capabilities);
}

std::optional<CapabilityReport> RenderService::backend_capabilities(std::string_view renderer) const {
    using render_common::EngineBackend;
    constexpr std::array<std::pair<std::string_view, EngineBackend>, 7> backends{{
        {"ear", EngineBackend::ear},
        {"saf", EngineBackend::saf},
        {"triple-balance", EngineBackend::triple_balance},
        {"hoa", EngineBackend::hoa},
        {"saf-binaural", EngineBackend::binaural},
        {"binaural", EngineBackend::binaural},
        {"room-binaural", EngineBackend::room},
    }};
    for (const auto& [name, backend] : backends) {
        if (name == renderer) {
            return render_common::engine_capabilities(backend);
        }
    }
#ifdef __APPLE__
    if (renderer == "apple") {
        return apple_capabilities();
    }
#endif
    return std::nullopt;
}

std::vector<OutputLayoutRow> RenderService::output_layouts() const {
    return engine::build_output_layouts();
}

std::string RenderService::layouts_json() const {
    return engine::document(engine::Document::layouts);
}

std::string RenderService::input_layouts_json() const {
    return engine::document(engine::Document::input_layouts);
}

OutputFormats RenderService::output_formats() const {
    return engine::build_output_formats();
}

std::string RenderService::output_formats_json() const {
    return engine::document(engine::Document::output_formats);
}

std::string RenderService::render_support_matrix_json() const {
    return engine::document(engine::Document::support_matrix);
}

Result<std::string> RenderService::axml(const std::string& input_path) const {
    return io::get_axml(input_path);
}

Result<std::string> RenderService::policy_template_json(const std::string& input_path) const {
    auto scene = io::RenderScene::import(input_path, nullptr);
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    return scene->policy_template();
}

Result<void> RenderService::write_policy_template(const std::filesystem::path& input_path,
                                                  const std::filesystem::path& output_path,
                                                  const RenderOptions& options) const {
    const io::SceneImportOptions import_options{options.input_layout, options.input_channel_labels};
    auto scene = io::RenderScene::import(input_path.string(), &import_options);
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    auto json = scene->policy_template();
    if (!json) {
        return tl::unexpected(json.error());
    }
    return io::write_policy_json(output_path, *json, "policy template");
}

Result<void> RenderService::export_file(const std::string& input_path,
                                        const std::string& output_path,
                                        const RenderOptions& options,
                                        LogSink& logs) const {
    auto scene = io::RenderScene::import(input_path, nullptr);
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    if (auto logged = log_import_warnings(*scene, logs); !logged) {
        return tl::unexpected(logged.error());
    }
    if (auto applied = apply_policy(*scene, input_path, options, logs); !applied) {
        return applied;
    }
    return scene->write(output_path);
}

Result<PreparedScene> RenderService::prepare_preview_scene(const std::filesystem::path& input_path,
                                                           const RenderOptions& options,
                                                           LogSink& logs) const {
    const io::SceneImportOptions import_options{options.input_layout, options.input_channel_labels};
    auto scene = io::RenderScene::import(input_path.string(), &import_options);
    if (!scene) {
        return tl::unexpected(scene.error());
    }
    if (auto logged = log_import_warnings(*scene, logs); !logged) {
        return tl::unexpected(logged.error());
    }
    if (auto applied = apply_policy(*scene, input_path.string(), options, logs); !applied) {
        return tl::unexpected(applied.error());
    }
    return PreparedScene{scene->handle()};
}

// ── PreviewSession ──────────────────────────────────────────────────────────────

PreviewSession::PreviewSession(std::filesystem::path input, RenderOptions options, PreparedScene scene)
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
