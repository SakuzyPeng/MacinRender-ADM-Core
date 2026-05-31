#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "adm/c_api.h"
#include "adm/render.h"

namespace {

const char* stage_name(mradm::RenderStage stage) noexcept {
    switch (stage) {
    case mradm::RenderStage::validating:
        return "validating";
    case mradm::RenderStage::probing:
        return "probing";
    case mradm::RenderStage::importing_scene:
        return "importing_scene";
    case mradm::RenderStage::planning:
        return "planning";
    case mradm::RenderStage::rendering:
        return "rendering";
    case mradm::RenderStage::post_processing:
        return "post_processing";
    case mradm::RenderStage::finished:
        return "finished";
    }
    return "unknown";
}

// Directly pin the C++ ErrorCode enum to the stable C ABI enum. errors.h asserts
// the ErrorCode values independently; these cross-checks ensure the two enums stay
// numerically identical, so a new error code cannot be added to one side only.
static_assert(static_cast<int>(mradm::ErrorCode::ok) == ADM_ERROR_OK);
static_assert(static_cast<int>(mradm::ErrorCode::invalid_argument) == ADM_ERROR_INVALID_ARGUMENT);
static_assert(static_cast<int>(mradm::ErrorCode::unsupported) == ADM_ERROR_UNSUPPORTED);
static_assert(static_cast<int>(mradm::ErrorCode::io_error) == ADM_ERROR_IO);
static_assert(static_cast<int>(mradm::ErrorCode::render_failed) == ADM_ERROR_RENDER_FAILED);
static_assert(static_cast<int>(mradm::ErrorCode::cancelled) == ADM_ERROR_CANCELLED);
static_assert(static_cast<int>(mradm::ErrorCode::internal_error) == ADM_ERROR_INTERNAL);

// v1.1 enum cross-checks.
static_assert(static_cast<int>(mradm::RendererSelection::automatic) == ADM_RENDERER_AUTOMATIC);
static_assert(static_cast<int>(mradm::RendererSelection::ear) == ADM_RENDERER_EAR);
static_assert(static_cast<int>(mradm::RendererSelection::saf) == ADM_RENDERER_SAF);
static_assert(static_cast<int>(mradm::RendererSelection::hoa) == ADM_RENDERER_HOA);
static_assert(static_cast<int>(mradm::RendererSelection::apple) == ADM_RENDERER_APPLE);
static_assert(static_cast<int>(mradm::RendererSelection::binaural) == ADM_RENDERER_BINAURAL);

static_assert(static_cast<int>(mradm::OutputBitDepth::f32) == ADM_BIT_DEPTH_F32);
static_assert(static_cast<int>(mradm::OutputBitDepth::i24) == ADM_BIT_DEPTH_I24);
static_assert(static_cast<int>(mradm::OutputBitDepth::i16) == ADM_BIT_DEPTH_I16);

static_assert(static_cast<int>(mradm::SpeakerSpreadMode::automatic) == ADM_SPEAKER_SPREAD_AUTOMATIC);
static_assert(static_cast<int>(mradm::SpeakerSpreadMode::none) == ADM_SPEAKER_SPREAD_NONE);
static_assert(static_cast<int>(mradm::SpeakerSpreadMode::mdap) == ADM_SPEAKER_SPREAD_MDAP);

static_assert(static_cast<int>(mradm::BinauralSpreadMode::automatic) == ADM_BINAURAL_SPREAD_AUTOMATIC);
static_assert(static_cast<int>(mradm::BinauralSpreadMode::none) == ADM_BINAURAL_SPREAD_NONE);
static_assert(static_cast<int>(mradm::BinauralSpreadMode::cloud) == ADM_BINAURAL_SPREAD_CLOUD);
static_assert(static_cast<int>(mradm::BinauralSpreadMode::saf_spreader) == ADM_BINAURAL_SPREAD_SAF_SPREADER);

static_assert(static_cast<int>(mradm::RenderOptions::IamfContainer::obu) == ADM_IAMF_CONTAINER_OBU);
static_assert(static_cast<int>(mradm::RenderOptions::IamfContainer::mp4) == ADM_IAMF_CONTAINER_MP4);

static_assert(static_cast<int>(mradm::LogLevel::debug) == ADM_LOG_DEBUG);
static_assert(static_cast<int>(mradm::LogLevel::info) == ADM_LOG_INFO);
static_assert(static_cast<int>(mradm::LogLevel::warning) == ADM_LOG_WARNING);
static_assert(static_cast<int>(mradm::LogLevel::error) == ADM_LOG_ERROR);

adm_log_level_t to_c_log_level(mradm::LogLevel level) noexcept {
    switch (level) {
    case mradm::LogLevel::debug:
        return ADM_LOG_DEBUG;
    case mradm::LogLevel::info:
        return ADM_LOG_INFO;
    case mradm::LogLevel::warning:
        return ADM_LOG_WARNING;
    case mradm::LogLevel::error:
        return ADM_LOG_ERROR;
    }
    return ADM_LOG_INFO;
}

adm_error_code_t map_error(mradm::ErrorCode code) noexcept {
    switch (code) {
    case mradm::ErrorCode::ok:
        return ADM_ERROR_OK;
    case mradm::ErrorCode::invalid_argument:
        return ADM_ERROR_INVALID_ARGUMENT;
    case mradm::ErrorCode::unsupported:
        return ADM_ERROR_UNSUPPORTED;
    case mradm::ErrorCode::io_error:
        return ADM_ERROR_IO;
    case mradm::ErrorCode::render_failed:
        return ADM_ERROR_RENDER_FAILED;
    case mradm::ErrorCode::cancelled:
        return ADM_ERROR_CANCELLED;
    case mradm::ErrorCode::internal_error:
        return ADM_ERROR_INTERNAL;
    }
    return ADM_ERROR_INTERNAL;
}

class CallbackProgressSink final : public mradm::ProgressSink {
  public:
    CallbackProgressSink(adm_progress_cb callback, void* user_data) : callback_(callback), user_data_(user_data) {}

    void on_progress(const mradm::ProgressEvent& event) override {
        if (callback_ == nullptr) {
            return;
        }
        const std::string message(event.message);
        callback_(event.fraction, stage_name(event.stage), message.c_str(), user_data_);
    }

  private:
    adm_progress_cb callback_{nullptr};
    void* user_data_{nullptr};
};

// Internal storage for a single captured diagnostic log line.
struct CLogEntry {
    adm_log_level_t level{ADM_LOG_INFO};
    std::string module;
    std::string message;
};

// LogSink that captures every log line into a caller-owned vector. The mutex is
// cheap insurance: renderer backends may log from worker threads (parallel
// binaural spreaders), and log calls are rare relative to per-sample work.
class CollectingLogSink final : public mradm::LogSink {
  public:
    explicit CollectingLogSink(std::vector<CLogEntry>& out) : out_(out) {}

    void log(mradm::LogLevel level, std::string_view module, std::string_view message) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        out_.push_back({to_c_log_level(level), std::string(module), std::string(message)});
    }

  private:
    std::vector<CLogEntry>& out_;
    std::mutex mutex_;
};

} // namespace

struct adm_context_t {
    mradm::RenderService service;
};

struct adm_render_result_t {
    adm_error_code_t code{ADM_ERROR_OK};
    std::string message;
    std::string output_path;             // v1.1
    std::optional<double> loudness_lufs; // v1.1
    std::optional<double> peak_dbtp;     // v1.1
    std::vector<CLogEntry> logs;         // v1.1
};

struct adm_render_options_t {
    mradm::RenderOptions opts;
};

struct adm_scene_info_t {
    uint32_t sample_rate{0};
    uint32_t channels{0};
    uint64_t frames{0};
    uint32_t programme_count{0};
    uint32_t object_count{0};
};

/* ── Version ──────────────────────────────────────────────────────────────── */

int adm_api_version_major(void) noexcept {
    return ADM_API_VERSION_MAJOR;
}
int adm_api_version_minor(void) noexcept {
    return ADM_API_VERSION_MINOR;
}
int adm_api_version_patch(void) noexcept {
    return ADM_API_VERSION_PATCH;
}

/* ── Context ──────────────────────────────────────────────────────────────── */

adm_context_t* adm_create_context(void) noexcept {
    try {
        return new adm_context_t{};
    } catch (...) {
        return nullptr;
    }
}

void adm_destroy_context(adm_context_t* context) noexcept {
    delete context;
}

/* ── Options builder ─────────────────────────────────────────────────────── */

adm_render_options_t* adm_create_render_options(void) noexcept {
    try {
        return new adm_render_options_t{};
    } catch (...) {
        return nullptr;
    }
}

void adm_destroy_render_options(adm_render_options_t* opts) noexcept {
    delete opts;
}

adm_error_code_t adm_render_options_set_renderer(adm_render_options_t* opts, adm_renderer_t renderer) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(renderer) < ADM_RENDERER_AUTOMATIC || static_cast<int>(renderer) > ADM_RENDERER_BINAURAL) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.renderer = static_cast<mradm::RendererSelection>(renderer);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_output_layout(adm_render_options_t* opts, const char* layout) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (layout == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        opts->opts.output_layout = layout;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_output_bit_depth(adm_render_options_t* opts,
                                                         adm_output_bit_depth_t depth) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(depth) < ADM_BIT_DEPTH_F32 || static_cast<int>(depth) > ADM_BIT_DEPTH_I16) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.output_bit_depth = static_cast<mradm::OutputBitDepth>(depth);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_loudness_target(adm_render_options_t* opts, double lufs) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (!std::isfinite(lufs) || lufs < -70.0 || lufs > 0.0) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.loudness_target_lufs = static_cast<float>(lufs);
    opts->opts.measure_loudness = true;
    return ADM_ERROR_OK;
}

void adm_render_options_set_peak_limit(adm_render_options_t* opts, int enabled) noexcept {
    if (opts == nullptr) {
        return;
    }
    opts->opts.peak_limit = (enabled != 0);
}

adm_error_code_t adm_render_options_set_peak_limit_dbtp(adm_render_options_t* opts, double dbtp) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (!std::isfinite(dbtp) || dbtp < -60.0 || dbtp > 0.0) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.peak_limit_dbtp = static_cast<float>(dbtp);
    return ADM_ERROR_OK;
}

void adm_render_options_set_peak_normalize_to_limit(adm_render_options_t* opts, int enabled) noexcept {
    if (opts == nullptr) {
        return;
    }
    opts->opts.peak_normalize_to_limit = (enabled != 0);
}

adm_error_code_t adm_render_options_set_opus_bitrate_per_ch_kbps(adm_render_options_t* opts, uint32_t kbps) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (kbps != 0 && (kbps < 6 || kbps > 320)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.opus_bitrate_per_ch_kbps = kbps;
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_apac_bitrate_kbps(adm_render_options_t* opts, uint32_t kbps) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (kbps != 0 && (kbps < 64 || kbps > 12000)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.apac_bitrate_kbps = kbps;
    return ADM_ERROR_OK;
}

void adm_render_options_set_apac_drc_music(adm_render_options_t* opts, int enabled) noexcept {
    if (opts == nullptr) {
        return;
    }
    opts->opts.apac_drc_music = (enabled != 0);
}

adm_error_code_t adm_render_options_set_sofa_path(adm_render_options_t* opts, const char* sofa_path) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (sofa_path == nullptr || sofa_path[0] == '\0') {
            opts->opts.sofa_path = std::nullopt;
        } else {
            opts->opts.sofa_path = std::filesystem::path{sofa_path};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_semantic_policy_path(adm_render_options_t* opts, const char* path) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (path == nullptr || path[0] == '\0') {
            opts->opts.semantic_policy_path = std::nullopt;
        } else {
            opts->opts.semantic_policy_path = std::filesystem::path{path};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_semantic_report_path(adm_render_options_t* opts, const char* path) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (path == nullptr || path[0] == '\0') {
            opts->opts.semantic_report_path = std::nullopt;
        } else {
            opts->opts.semantic_report_path = std::filesystem::path{path};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_default_interp_ms(adm_render_options_t* opts, uint32_t ms) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (ms > 500) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.default_interp_ms = ms;
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_object_smoothing_frames(adm_render_options_t* opts, uint32_t frames) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (frames > 48000) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.object_smoothing_frames = frames;
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_speaker_spread_mode(adm_render_options_t* opts,
                                                            adm_speaker_spread_mode_t mode) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(mode) < ADM_SPEAKER_SPREAD_AUTOMATIC || static_cast<int>(mode) > ADM_SPEAKER_SPREAD_MDAP) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.speaker_spread_mode = static_cast<mradm::SpeakerSpreadMode>(mode);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_binaural_spread_mode(adm_render_options_t* opts,
                                                             adm_binaural_spread_mode_t mode) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(mode) < ADM_BINAURAL_SPREAD_AUTOMATIC ||
        static_cast<int>(mode) > ADM_BINAURAL_SPREAD_SAF_SPREADER) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.binaural_spread_mode = static_cast<mradm::BinauralSpreadMode>(mode);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_iamf_container(adm_render_options_t* opts,
                                                       adm_iamf_container_t container) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(container) < ADM_IAMF_CONTAINER_OBU || static_cast<int>(container) > ADM_IAMF_CONTAINER_MP4) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.iamf_container = static_cast<mradm::RenderOptions::IamfContainer>(container);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_render_start_sec(adm_render_options_t* opts, double sec) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (!std::isfinite(sec) || sec < 0.0) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.render_start_sec = sec;
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_render_end_sec(adm_render_options_t* opts, double sec) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (!std::isfinite(sec)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    // sec <= 0 clears the end (render to the end); a positive end is validated
    // against start at render time.
    if (sec <= 0.0) {
        opts->opts.render_end_sec = std::nullopt;
    } else {
        opts->opts.render_end_sec = sec;
    }
    return ADM_ERROR_OK;
}

/* ── Render ──────────────────────────────────────────────────────────────── */

adm_error_code_t adm_render_file_ex(adm_context_t* context,
                                    const char* input_path,
                                    const char* output_path,
                                    const adm_render_options_t* opts,
                                    adm_progress_cb progress,
                                    void* user_data,
                                    adm_render_result_t** result) noexcept {
    if (result != nullptr) {
        *result = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        mradm::RenderRequest request;
        request.input_path = input_path;
        if (output_path != nullptr && output_path[0] != '\0') {
            request.output_path = output_path;
        }
        if (opts != nullptr) {
            request.options = opts->opts;
        }

        CallbackProgressSink progress_sink(progress, user_data);
        // Only collect logs when the caller asked for a result handle to read them from.
        std::vector<CLogEntry> captured;
        CollectingLogSink collecting(captured);
        mradm::NullLogSink null_sink;
        mradm::LogSink& log_sink =
            (result != nullptr) ? static_cast<mradm::LogSink&>(collecting) : static_cast<mradm::LogSink&>(null_sink);
        mradm::RenderResult cpp_result = context->service.render(request, progress_sink, log_sink);

        const adm_error_code_t code = map_error(cpp_result.error.code);
        if (result == nullptr) {
            return code;
        }

        auto c_result = std::unique_ptr<adm_render_result_t>(new (std::nothrow) adm_render_result_t{});
        if (!c_result) {
            return ADM_ERROR_INTERNAL;
        }
        c_result->code = code;
        c_result->message = cpp_result.error.message;
        if (cpp_result.output_path.has_value()) {
            c_result->output_path = cpp_result.output_path->string();
        }
        if (cpp_result.metrics.has_value()) {
            c_result->loudness_lufs = cpp_result.metrics->measured_lufs;
            c_result->peak_dbtp = cpp_result.metrics->measured_peak_dbtp;
        }
        c_result->logs = std::move(captured);
        *result = c_result.release();

        return code;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 adm_progress_cb progress,
                                 void* user_data,
                                 adm_render_result_t** result) noexcept {
    return adm_render_file_ex(context, input_path, output_path, nullptr, progress, user_data, result);
}

/* ── Result ──────────────────────────────────────────────────────────────── */

void adm_destroy_render_result(adm_render_result_t* result) noexcept {
    delete result;
}

adm_error_code_t adm_render_result_error_code(const adm_render_result_t* result) noexcept {
    if (result == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    return result->code;
}

const char* adm_render_result_message(const adm_render_result_t* result) noexcept {
    if (result == nullptr) {
        return "result is null";
    }
    return result->message.c_str();
}

const char* adm_render_result_output_path(const adm_render_result_t* result) noexcept {
    if (result == nullptr) {
        return nullptr;
    }
    return result->output_path.c_str();
}

int adm_render_result_loudness_lufs(const adm_render_result_t* result, double* out_value) noexcept {
    if (result == nullptr || !result->loudness_lufs.has_value()) {
        return 0;
    }
    if (out_value != nullptr) {
        *out_value = *result->loudness_lufs;
    }
    return 1;
}

int adm_render_result_peak_dbtp(const adm_render_result_t* result, double* out_value) noexcept {
    if (result == nullptr || !result->peak_dbtp.has_value()) {
        return 0;
    }
    if (out_value != nullptr) {
        *out_value = *result->peak_dbtp;
    }
    return 1;
}

uint32_t adm_render_result_log_count(const adm_render_result_t* result) noexcept {
    if (result == nullptr) {
        return 0;
    }
    // Saturate rather than truncate: a count narrowed by the high bits would make
    // the caller stop iterating early and silently miss the remaining entries.
    const std::size_t n = result->logs.size();
    return (n > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(n);
}

int adm_render_result_log_entry(const adm_render_result_t* result,
                                uint32_t index,
                                adm_log_level_t* out_level,
                                const char** out_module,
                                const char** out_message) noexcept {
    if (result == nullptr || index >= result->logs.size()) {
        return 0;
    }
    const CLogEntry& entry = result->logs[index];
    if (out_level != nullptr) {
        *out_level = entry.level;
    }
    if (out_module != nullptr) {
        *out_module = entry.module.c_str();
    }
    if (out_message != nullptr) {
        *out_message = entry.message.c_str();
    }
    return 1;
}

/* ── Probe ───────────────────────────────────────────────────────────────── */

adm_error_code_t adm_probe_file(adm_context_t* context, const char* input_path, adm_scene_info_t** out) noexcept {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto probe_result = context->service.probe(input_path);
        if (!probe_result) {
            return map_error(probe_result.error().code);
        }

        if (out == nullptr) {
            return ADM_ERROR_OK;
        }

        // cppcheck-suppress unreadVariable -- info is read via *out = info below
        auto* info = new (std::nothrow) adm_scene_info_t{};
        if (info == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        info->sample_rate = probe_result->sample_rate;
        info->channels = probe_result->num_channels;
        info->frames = probe_result->num_frames;
        info->programme_count = probe_result->programme_count;
        info->object_count = probe_result->object_count;
        *out = info;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

void adm_destroy_scene_info(adm_scene_info_t* info) noexcept {
    delete info;
}

uint32_t adm_scene_info_sample_rate(const adm_scene_info_t* info) noexcept {
    return info != nullptr ? info->sample_rate : 0U;
}

uint32_t adm_scene_info_channels(const adm_scene_info_t* info) noexcept {
    return info != nullptr ? info->channels : 0U;
}

uint64_t adm_scene_info_frames(const adm_scene_info_t* info) noexcept {
    return info != nullptr ? info->frames : 0ULL;
}

double adm_scene_info_duration_seconds(const adm_scene_info_t* info) noexcept {
    if (info == nullptr || info->sample_rate == 0U) {
        return 0.0;
    }
    return static_cast<double>(info->frames) / static_cast<double>(info->sample_rate);
}

uint32_t adm_scene_info_programme_count(const adm_scene_info_t* info) noexcept {
    return info != nullptr ? info->programme_count : 0U;
}

uint32_t adm_scene_info_object_count(const adm_scene_info_t* info) noexcept {
    return info != nullptr ? info->object_count : 0U;
}

/* ── Scene inspect (JSON) ────────────────────────────────────────────────── */

adm_error_code_t adm_inspect_file_json(adm_context_t* context, const char* input_path, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto json_result = context->service.inspect_json(input_path);
        if (!json_result) {
            return map_error(json_result.error().code);
        }
        if (out_json == nullptr) {
            return ADM_ERROR_OK; // validate-only: parsed successfully, allocate nothing
        }
        const std::string& json = *json_result;
        auto* buffer = new (std::nothrow) char[json.size() + 1];
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, json.c_str(), json.size() + 1);
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_inspect_file_xml(adm_context_t* context, const char* input_path, char** out_xml) noexcept {
    if (out_xml != nullptr) {
        *out_xml = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto xml_result = context->service.axml(input_path);
        if (!xml_result) {
            return map_error(xml_result.error().code);
        }
        if (out_xml == nullptr) {
            return ADM_ERROR_OK; // validate-only: chunk readable, allocate nothing
        }
        const std::string& xml = *xml_result;
        auto* buffer = new (std::nothrow) char[xml.size() + 1];
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, xml.c_str(), xml.size() + 1);
        *out_xml = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_policy_template_json(adm_context_t* context, const char* input_path, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto tmpl_result = context->service.policy_template_json(input_path);
        if (!tmpl_result) {
            return map_error(tmpl_result.error().code);
        }
        if (out_json == nullptr) {
            return ADM_ERROR_OK; // validate-only: scene parsed, allocate nothing
        }
        const std::string& tmpl = *tmpl_result;
        auto* buffer = new (std::nothrow) char[tmpl.size() + 1];
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, tmpl.c_str(), tmpl.size() + 1);
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

// Takes ownership of a char* the ABI handed out via char** out-params; the
// non-const pointer type is part of the contract, so the const-pointer hint
// does not apply here.
// NOLINTNEXTLINE(readability-non-const-parameter)
void adm_free_string(char* s) noexcept {
    delete[] s;
}

/* ── Capabilities (JSON) ─────────────────────────────────────────────────── */

adm_error_code_t adm_capabilities_json(adm_context_t* context, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        const std::string json = context->service.capabilities_json();
        auto* buffer = new (std::nothrow) char[json.size() + 1];
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, json.c_str(), json.size() + 1);
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_layouts_json(adm_context_t* context, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        const std::string json = context->service.layouts_json();
        auto* buffer = new (std::nothrow) char[json.size() + 1];
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, json.c_str(), json.size() + 1);
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
