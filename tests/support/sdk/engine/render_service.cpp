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

#include "adm_apple/apple_island.h"
#endif
#include "../io/scene_import_internal.h"
#include "../room/room_internal.h"
#include "adm_audio/island_ffi.h"
#include "capability_records.h"
#include "catalog.h"
#include "engine_records.h"
#include "engine_result.h"
#include "platform_island.h"
#include "request_record.h"
#include "scene_handle.h"

namespace mradm {

namespace {

using engine::engine_path;
using engine::RequestTexts;
using engine::utf8;
using render_common::engine_text;

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

// The caller's stop token, progress and logs for the islands the engine runs (mradm_audio_island
// and the Apple islands).
struct IslandDriver {
    const std::stop_token* token{nullptr};
    ProgressSink* progress{nullptr};
    LogSink* logs{nullptr};
    std::exception_ptr failure = nullptr;
    std::atomic<bool> failed{false};
};

// cppcheck-suppress constParameterCallback ; matches the private C callback signature.
uint32_t island_cancelled(void* user) noexcept {
    const auto& driver = *static_cast<const IslandDriver*>(user);
    return driver.failed.load() || driver.token->stop_requested() ? 1U : 0U;
}

// Keeps a caller exception on the C++ side of the Rust callback boundary.
template <class F> void island_call(IslandDriver& driver, const F& f) noexcept {
    if (driver.failed.load()) {
        return;
    }
    try {
        f();
    } catch (...) {
        driver.failure = std::current_exception();
        driver.failed.store(true);
    }
}

void island_progress(void* user, const MradmEngineProgress* event) noexcept {
    auto& driver = *static_cast<IslandDriver*>(user);
    island_call(driver, [&] { driver.progress->on_progress(progress_of(*event)); });
}

void island_log(void* user, uint32_t level, MradmAdmText module, MradmAdmText message) noexcept {
    auto& driver = *static_cast<IslandDriver*>(user);
    island_call(driver,
                [&] { driver.logs->log(static_cast<LogLevel>(level), engine_text(module), engine_text(message)); });
}

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
// loop forwards its progress and logs and passes the stop token before every step; the engine
// runs the islands (the audio library's and the Apple backend's) inside the step.
RenderResult RenderService::render(const RenderRequest& request,
                                   ProgressSink& progress,
                                   LogSink& logs,
                                   const PreparedScene* preimported_scene,
                                   std::shared_ptr<IPreparedRender>* prepared_cache) const {
    const RequestTexts texts(request);
    const auto record = texts.record(request);
    std::shared_ptr<IPreparedRender> local;
    auto& slot = prepared_cache != nullptr ? *prepared_cache : local;
    const auto* cached = dynamic_cast<const render_common::EnginePrepared*>(slot.get());
    MradmEngineRender* raw = nullptr;
    const auto code = mradm_engine_render_begin(&record,
                                                mradm_platform_islands(),
                                                preimported_scene != nullptr ? preimported_scene->handle->raw : nullptr,
                                                cached != nullptr ? cached->handle.get() : nullptr,
                                                &raw);
    if (code != 0) {
        return {
            error_of(code, engine_text("invalid render request (UTF-8 paths without NUL required)"), {}), {}, {}, {}};
    }
    const RenderHandle render(raw, &mradm_engine_render_destroy);
    IslandDriver driver{&request.options.cancel_token, &progress, &logs};
#ifdef __APPLE__
    const MradmAppleIslands* apple = mradm_apple_islands();
#else
    const MradmAppleIslands* apple = nullptr;
#endif
    const MradmIslandRunner runner{
        &mradm_audio_island, {&driver, &island_cancelled, &island_progress, &island_log}, apple};
    if (mradm_engine_render_set_islands(render.get(), &runner) != 0) {
        return {{ErrorCode::internal_error, "render islands unavailable", {}}, std::nullopt, std::nullopt, {}};
    }
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
            // Every island runs inside the step; one returned here has no runner in this build.
            MradmEngineIslandReply reply{};
            reply.code = static_cast<int32_t>(ErrorCode::internal_error);
            reply.message = engine_text("unknown render island");
            reply.ffmpeg_major = -1;
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
