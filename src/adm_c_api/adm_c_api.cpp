#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "adm/c_api.h"
#include "adm/monitor.h"
#include "adm/render.h"

#include "scene_stream_engine.h"

namespace {

std::vector<std::string> split_csv_list(std::string_view csv, bool preserve_empty = false) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        const std::size_t comma = csv.find(',', pos);
        const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
        std::string_view item = csv.substr(pos, end - pos);
        const auto first = item.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos) {
            const auto last = item.find_last_not_of(" \t\r\n");
            out.emplace_back(item.substr(first, last - first + 1));
        } else if (preserve_empty) {
            out.emplace_back();
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return out;
}

// These stage strings are part of the progress callback's documented contract.
// adm_render_stage_from_string() must recognize every string produced here; the
// progress-callback test renders end-to-end and asserts no emitted stage maps to
// ADM_STAGE_UNKNOWN, guarding the two against drift.
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

adm_render_stage_t to_c_stage(mradm::RenderStage stage) noexcept {
    switch (stage) {
    case mradm::RenderStage::validating:
        return ADM_STAGE_VALIDATING;
    case mradm::RenderStage::probing:
        return ADM_STAGE_PROBING;
    case mradm::RenderStage::importing_scene:
        return ADM_STAGE_IMPORTING_SCENE;
    case mradm::RenderStage::planning:
        return ADM_STAGE_PLANNING;
    case mradm::RenderStage::rendering:
        return ADM_STAGE_RENDERING;
    case mradm::RenderStage::post_processing:
        return ADM_STAGE_POST_PROCESSING;
    case mradm::RenderStage::finished:
        return ADM_STAGE_FINISHED;
    }
    return ADM_STAGE_UNKNOWN;
}

adm_progress_operation_t to_c_operation(mradm::RenderOperation operation) noexcept {
    switch (operation) {
    case mradm::RenderOperation::unknown:
        return ADM_PROGRESS_OPERATION_UNKNOWN;
    case mradm::RenderOperation::validate_request:
        return ADM_PROGRESS_OPERATION_VALIDATE_REQUEST;
    case mradm::RenderOperation::probe_input:
        return ADM_PROGRESS_OPERATION_PROBE_INPUT;
    case mradm::RenderOperation::import_scene:
        return ADM_PROGRESS_OPERATION_IMPORT_SCENE;
    case mradm::RenderOperation::apply_semantic_policy:
        return ADM_PROGRESS_OPERATION_APPLY_SEMANTIC_POLICY;
    case mradm::RenderOperation::plan_render:
        return ADM_PROGRESS_OPERATION_PLAN_RENDER;
    case mradm::RenderOperation::prepare_backend:
        return ADM_PROGRESS_OPERATION_PREPARE_BACKEND;
    case mradm::RenderOperation::render_audio:
        return ADM_PROGRESS_OPERATION_RENDER_AUDIO;
    case mradm::RenderOperation::trim_output:
        return ADM_PROGRESS_OPERATION_TRIM_OUTPUT;
    case mradm::RenderOperation::apply_gain:
        return ADM_PROGRESS_OPERATION_APPLY_GAIN;
    case mradm::RenderOperation::convert_bit_depth:
        return ADM_PROGRESS_OPERATION_CONVERT_BIT_DEPTH;
    case mradm::RenderOperation::encode_flac:
        return ADM_PROGRESS_OPERATION_ENCODE_FLAC;
    case mradm::RenderOperation::encode_opus:
        return ADM_PROGRESS_OPERATION_ENCODE_OPUS;
    case mradm::RenderOperation::encode_apac:
        return ADM_PROGRESS_OPERATION_ENCODE_APAC;
    case mradm::RenderOperation::encode_iamf:
        return ADM_PROGRESS_OPERATION_ENCODE_IAMF;
    case mradm::RenderOperation::package_iamf_mp4:
        return ADM_PROGRESS_OPERATION_PACKAGE_IAMF_MP4;
    case mradm::RenderOperation::write_metadata:
        return ADM_PROGRESS_OPERATION_WRITE_METADATA;
    case mradm::RenderOperation::finish:
        return ADM_PROGRESS_OPERATION_FINISH;
    }
    return ADM_PROGRESS_OPERATION_UNKNOWN;
}

[[nodiscard]] double clamp_fraction(double value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 1.0);
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
static_assert(static_cast<int>(mradm::RendererSelection::saf_binaural) == ADM_RENDERER_SAF_BINAURAL);

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

static_assert(static_cast<int>(mradm::LfeRoutingMode::direct) == ADM_LFE_ROUTING_DIRECT);
static_assert(static_cast<int>(mradm::LfeRoutingMode::split_power) == ADM_LFE_ROUTING_SPLIT_POWER);

static_assert(static_cast<int>(mradm::SpeakerGeometry::standard) == ADM_SPEAKER_GEOMETRY_STANDARD);
static_assert(static_cast<int>(mradm::SpeakerGeometry::apple) == ADM_SPEAKER_GEOMETRY_APPLE);

static_assert(static_cast<int>(mradm::DirectSpeakersRoutingMode::automatic) == ADM_DIRECT_SPEAKERS_ROUTING_AUTOMATIC);
static_assert(static_cast<int>(mradm::DirectSpeakersRoutingMode::label) == ADM_DIRECT_SPEAKERS_ROUTING_LABEL);
static_assert(static_cast<int>(mradm::DirectSpeakersRoutingMode::position) == ADM_DIRECT_SPEAKERS_ROUTING_POSITION);
static_assert(static_cast<int>(mradm::DirectSpeakersRoutingMode::matrix) == ADM_DIRECT_SPEAKERS_ROUTING_MATRIX);

static_assert(static_cast<int>(mradm::RenderOptions::IamfContainer::obu) == ADM_IAMF_CONTAINER_OBU);
static_assert(static_cast<int>(mradm::RenderOptions::IamfContainer::mp4) == ADM_IAMF_CONTAINER_MP4);
static_assert(static_cast<int>(mradm::RenderOptions::ApacContainer::mpeg4) == ADM_APAC_CONTAINER_MPEG4);
static_assert(static_cast<int>(mradm::RenderOptions::ApacContainer::caf) == ADM_APAC_CONTAINER_CAF);

static_assert(static_cast<int>(mradm::LogLevel::debug) == ADM_LOG_DEBUG);
static_assert(static_cast<int>(mradm::LogLevel::info) == ADM_LOG_INFO);
static_assert(static_cast<int>(mradm::LogLevel::warning) == ADM_LOG_WARNING);
static_assert(static_cast<int>(mradm::LogLevel::error) == ADM_LOG_ERROR);

static_assert(static_cast<int>(mradm::RenderOperation::unknown) == ADM_PROGRESS_OPERATION_UNKNOWN);
static_assert(static_cast<int>(mradm::RenderOperation::validate_request) == ADM_PROGRESS_OPERATION_VALIDATE_REQUEST);
static_assert(static_cast<int>(mradm::RenderOperation::probe_input) == ADM_PROGRESS_OPERATION_PROBE_INPUT);
static_assert(static_cast<int>(mradm::RenderOperation::import_scene) == ADM_PROGRESS_OPERATION_IMPORT_SCENE);
static_assert(static_cast<int>(mradm::RenderOperation::apply_semantic_policy) ==
              ADM_PROGRESS_OPERATION_APPLY_SEMANTIC_POLICY);
static_assert(static_cast<int>(mradm::RenderOperation::plan_render) == ADM_PROGRESS_OPERATION_PLAN_RENDER);
static_assert(static_cast<int>(mradm::RenderOperation::prepare_backend) == ADM_PROGRESS_OPERATION_PREPARE_BACKEND);
static_assert(static_cast<int>(mradm::RenderOperation::render_audio) == ADM_PROGRESS_OPERATION_RENDER_AUDIO);
static_assert(static_cast<int>(mradm::RenderOperation::trim_output) == ADM_PROGRESS_OPERATION_TRIM_OUTPUT);
static_assert(static_cast<int>(mradm::RenderOperation::apply_gain) == ADM_PROGRESS_OPERATION_APPLY_GAIN);
static_assert(static_cast<int>(mradm::RenderOperation::convert_bit_depth) == ADM_PROGRESS_OPERATION_CONVERT_BIT_DEPTH);
static_assert(static_cast<int>(mradm::RenderOperation::encode_flac) == ADM_PROGRESS_OPERATION_ENCODE_FLAC);
static_assert(static_cast<int>(mradm::RenderOperation::encode_opus) == ADM_PROGRESS_OPERATION_ENCODE_OPUS);
static_assert(static_cast<int>(mradm::RenderOperation::encode_apac) == ADM_PROGRESS_OPERATION_ENCODE_APAC);
static_assert(static_cast<int>(mradm::RenderOperation::encode_iamf) == ADM_PROGRESS_OPERATION_ENCODE_IAMF);
static_assert(static_cast<int>(mradm::RenderOperation::package_iamf_mp4) == ADM_PROGRESS_OPERATION_PACKAGE_IAMF_MP4);
static_assert(static_cast<int>(mradm::RenderOperation::write_metadata) == ADM_PROGRESS_OPERATION_WRITE_METADATA);
static_assert(static_cast<int>(mradm::RenderOperation::finish) == ADM_PROGRESS_OPERATION_FINISH);

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
        callback_(clamp_fraction(event.fraction), stage_name(event.stage), message.c_str(), user_data_);
    }

  private:
    adm_progress_cb callback_{nullptr};
    void* user_data_{nullptr};
};

class CallbackProgressSinkV2 final : public mradm::ProgressSink {
  public:
    CallbackProgressSinkV2(adm_progress_v2_cb callback, void* user_data) : callback_(callback), user_data_(user_data) {}

    void on_progress(const mradm::ProgressEvent& event) override {
        if (callback_ == nullptr) {
            return;
        }
        const std::string message(event.message);
        adm_progress_event_v2_t c_event{};
        c_event.struct_size = static_cast<uint32_t>(sizeof(adm_progress_event_v2_t));
        c_event.stage = to_c_stage(event.stage);
        c_event.operation = to_c_operation(event.operation);
        c_event.overall_fraction = clamp_fraction(event.fraction);
        c_event.stage_fraction = clamp_fraction(event.stage_fraction);
        c_event.current_frame = event.current_frame;
        c_event.total_frames = event.total_frames;
        c_event.message = message.c_str();
        callback_(&c_event, user_data_);
    }

  private:
    adm_progress_v2_cb callback_{nullptr};
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
    std::string last_error_message;
};

struct adm_cancel_token_t {
    // request_stop() (via adm_cancel) is safe to call from a thread other than the
    // one running the render that holds a token from this source. Resetting swaps
    // in a fresh source; the engine always pulls a token at render start.
    std::stop_source source;
};

struct adm_render_result_t {
    adm_error_code_t code{ADM_ERROR_OK};
    std::string message;
    std::string output_path;                         // v1.1
    std::optional<double> loudness_lufs;             // v1.1
    std::optional<double> peak_dbtp;                 // v1.1
    std::vector<CLogEntry> logs;                     // v1.1
    std::optional<std::string> semantic_report_json; // v1.5
};

struct adm_render_options_t {
    mradm::RenderOptions opts;
    // Borrowed (non-owning) cancel token. The live std::stop_token is resolved
    // from this at render time so that adm_reset_cancel_token (which swaps in a
    // fresh stop_source) is always observed. nullptr = non-cancellable.
    adm_cancel_token_t* cancel_token{nullptr};
};

struct adm_preview_session_t {
    mradm::PreviewSession session;
};

namespace {

void clear_last_error(adm_context_t* context) {
    if (context != nullptr) {
        context->last_error_message.clear();
    }
}

void store_last_error(adm_context_t* context, const mradm::Error& error) {
    if (context == nullptr) {
        return;
    }

    context->last_error_message = error.message;
    if (!error.context.empty()) {
        context->last_error_message += ": ";
        context->last_error_message += error.context;
    }
}

} // namespace

struct adm_monitor_t {
    std::unique_ptr<mradm::MonitorSession> session;
    std::string last_error_message;
    // Scratch storage backing the const char* returned by adm_monitor_log_entry; valid
    // until the next adm_monitor_* call (as the header documents).
    std::string log_module;
    std::string log_message;
};

struct adm_scene_stream_t {
    std::unique_ptr<mradm::realtime::SceneStreamEngine> engine;
    mutable std::mutex message_mutex;
    std::string last_error_message;
    // Backs adm_scene_diagnostic_t::message until another non-pull stream call.
    std::string diagnostic_message;
};

namespace {

void clear_last_error(adm_monitor_t* monitor) {
    if (monitor != nullptr) {
        monitor->last_error_message.clear();
    }
}

void store_last_error(adm_monitor_t* monitor, const mradm::Error& error) {
    if (monitor == nullptr) {
        return;
    }

    monitor->last_error_message = error.message;
    if (!error.context.empty()) {
        monitor->last_error_message += ": ";
        monitor->last_error_message += error.context;
    }
}

void clear_last_error(adm_scene_stream_t* stream) {
    if (stream != nullptr) {
        const std::lock_guard<std::mutex> lock(stream->message_mutex);
        stream->last_error_message.clear();
        stream->diagnostic_message.clear();
    }
}

void store_last_error_message(adm_scene_stream_t* stream, std::string message) {
    if (stream == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(stream->message_mutex);
    stream->last_error_message = std::move(message);
}

void store_last_error(adm_scene_stream_t* stream, const mradm::Error& error) {
    std::string message = error.message;
    if (!error.context.empty()) {
        message += ": ";
        message += error.context;
    }
    store_last_error_message(stream, std::move(message));
}

template <typename T>
bool load_sized_array(const T* source, uint32_t count, std::size_t minimum_size, std::vector<T>& destination) {
    destination.clear();
    if (count == 0U) {
        return true;
    }
    if (source == nullptr) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(source);
    uint32_t stride = 0U;
    std::memcpy(&stride, bytes, sizeof(stride));
    if (stride < minimum_size ||
        static_cast<std::size_t>(count) > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(stride)) {
        return false;
    }
    destination.reserve(count);
    for (uint32_t index = 0U; index < count; ++index) {
        const auto* element = bytes + (static_cast<std::size_t>(index) * stride);
        uint32_t element_size = 0U;
        std::memcpy(&element_size, element, sizeof(element_size));
        if (element_size != stride) {
            return false;
        }
        T value{};
        std::memcpy(&value, element, std::min<std::size_t>(sizeof(value), stride));
        destination.push_back(value);
    }
    return true;
}

template <typename T> bool output_struct_valid(const T* output) noexcept {
    return output != nullptr && output->struct_size >= sizeof(uint32_t);
}

template <typename T> void write_sized_output(T* output, T value) noexcept {
    const uint32_t caller_size = output->struct_size;
    value.struct_size = caller_size;
    std::memcpy(output, &value, std::min<std::size_t>(caller_size, sizeof(value)));
}

mradm::live_scene::ObjectState to_scene_state(const adm_scene_object_state_t& state) noexcept {
    mradm::live_scene::ObjectState converted;
    converted.valid_fields = state.valid_fields;
    converted.active = state.active != 0;
    converted.linear_gain = state.linear_gain;
    converted.x = state.position_x;
    converted.y = state.position_y;
    converted.z = state.position_z;
    converted.width = state.extent_width;
    converted.height = state.extent_height;
    converted.depth = state.extent_depth;
    converted.diffuse = state.diffuse;
    converted.divergence = state.divergence;
    converted.channel_lock = state.channel_lock != 0;
    converted.screen_reference = state.screen_reference != 0;
    converted.head_locked = state.head_locked != 0;
    return converted;
}

static_assert(static_cast<int>(mradm::live_scene::ElementRole::object) == ADM_SCENE_ELEMENT_OBJECT);
static_assert(static_cast<int>(mradm::live_scene::ElementRole::direct_speaker) == ADM_SCENE_ELEMENT_DIRECT_SPEAKER);
static_assert(static_cast<int>(mradm::live_scene::ElementRole::lfe) == ADM_SCENE_ELEMENT_LFE);
static_assert(static_cast<int>(mradm::realtime::SceneSubmitStatus::accepted) == ADM_SCENE_SUBMIT_ACCEPTED);
static_assert(static_cast<int>(mradm::realtime::SceneSubmitStatus::would_block) == ADM_SCENE_SUBMIT_WOULD_BLOCK);
static_assert(static_cast<int>(mradm::realtime::SceneSubmitStatus::timed_out) == ADM_SCENE_SUBMIT_TIMED_OUT);
static_assert(static_cast<int>(mradm::realtime::SceneSubmitStatus::closed) == ADM_SCENE_SUBMIT_CLOSED);
static_assert(static_cast<int>(mradm::realtime::SceneStreamState::idle) == ADM_SCENE_STREAM_IDLE);
static_assert(static_cast<int>(mradm::realtime::SceneStreamState::buffering) == ADM_SCENE_STREAM_BUFFERING);
static_assert(static_cast<int>(mradm::realtime::SceneStreamState::running) == ADM_SCENE_STREAM_RUNNING);
static_assert(static_cast<int>(mradm::realtime::SceneStreamState::draining) == ADM_SCENE_STREAM_DRAINING);
static_assert(static_cast<int>(mradm::realtime::SceneStreamState::ended) == ADM_SCENE_STREAM_ENDED);
static_assert(static_cast<int>(mradm::realtime::SceneStreamState::failed) == ADM_SCENE_STREAM_FAILED);

// Build an owned C result handle from a finished render. Shared by adm_render_file_ex
// and adm_preview_render_window. Returns nullptr on allocation failure.
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
adm_render_result_t* make_c_result(mradm::RenderResult&& cpp_result, std::vector<CLogEntry>&& captured) {
    // cppcheck-suppress unreadVariable -- c is used via operator-> and release()
    auto c = std::unique_ptr<adm_render_result_t>(new (std::nothrow) adm_render_result_t{});
    if (!c) {
        return nullptr;
    }
    c->code = map_error(cpp_result.error.code);
    c->message = cpp_result.error.message;
    if (cpp_result.output_path.has_value()) {
        c->output_path = cpp_result.output_path->string();
    }
    if (cpp_result.metrics.has_value()) {
        c->loudness_lufs = cpp_result.metrics->measured_lufs;
        c->peak_dbtp = cpp_result.metrics->measured_peak_dbtp;
    }
    c->semantic_report_json = std::move(cpp_result.semantic_report_json);
    c->logs = std::move(captured);
    return c.release();
}

} // namespace

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

/* ── Progress stage (v1.7) ──────────────────────────────────────────────────── */

adm_render_stage_t adm_render_stage_from_string(const char* stage) noexcept {
    if (stage == nullptr) {
        return ADM_STAGE_UNKNOWN;
    }
    // Must mirror stage_name() above (kept in sync via the progress-callback test).
    const std::string_view s{stage};
    if (s == "validating") {
        return ADM_STAGE_VALIDATING;
    }
    if (s == "probing") {
        return ADM_STAGE_PROBING;
    }
    if (s == "importing_scene") {
        return ADM_STAGE_IMPORTING_SCENE;
    }
    if (s == "planning") {
        return ADM_STAGE_PLANNING;
    }
    if (s == "rendering") {
        return ADM_STAGE_RENDERING;
    }
    if (s == "post_processing") {
        return ADM_STAGE_POST_PROCESSING;
    }
    if (s == "finished") {
        return ADM_STAGE_FINISHED;
    }
    return ADM_STAGE_UNKNOWN;
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

const char* adm_context_last_error_message(const adm_context_t* context) noexcept {
    return context != nullptr ? context->last_error_message.c_str() : "";
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
    if (static_cast<int>(renderer) < ADM_RENDERER_AUTOMATIC || static_cast<int>(renderer) > ADM_RENDERER_SAF_BINAURAL) {
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

adm_error_code_t adm_render_options_set_input_layout(adm_render_options_t* opts, const char* layout) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (layout == nullptr || layout[0] == '\0' || std::string_view{layout} == "auto") {
            opts->opts.input_layout = std::nullopt;
        } else {
            opts->opts.input_layout = std::string{layout};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_input_channel_labels(adm_render_options_t* opts,
                                                             const char* labels_csv) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (labels_csv == nullptr || labels_csv[0] == '\0') {
            opts->opts.input_channel_labels.clear();
        } else {
            opts->opts.input_channel_labels = split_csv_list(labels_csv, true);
        }
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

void adm_render_options_set_monitor_system_spatial(adm_render_options_t* opts, int enabled) noexcept {
    if (opts == nullptr) {
        return;
    }
    opts->opts.monitor_system_spatial = (enabled != 0);
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
    if (kbps != 0 && (kbps < 64 || kbps > 32768)) {
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

void adm_render_options_set_apple_speaker_rendering_flags(adm_render_options_t* opts, int enabled) noexcept {
    if (opts == nullptr) {
        return;
    }
    opts->opts.apple_speaker_rendering_flags = (enabled != 0);
}

adm_error_code_t adm_render_options_set_apac_container(adm_render_options_t* opts,
                                                       adm_apac_container_t container) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(container) < ADM_APAC_CONTAINER_MPEG4 ||
        static_cast<int>(container) > ADM_APAC_CONTAINER_CAF) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.apac_container = static_cast<mradm::RenderOptions::ApacContainer>(container);
    return ADM_ERROR_OK;
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

adm_error_code_t adm_render_options_set_semantic_policy_json(adm_render_options_t* opts, const char* json) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (json == nullptr || json[0] == '\0') {
            opts->opts.semantic_policy_json = std::nullopt;
        } else {
            opts->opts.semantic_policy_json = std::string{json};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

void adm_render_options_set_capture_semantic_report(adm_render_options_t* opts, int enabled) noexcept {
    if (opts != nullptr) {
        opts->opts.capture_semantic_report = (enabled != 0);
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

adm_error_code_t adm_render_options_set_speaker_geometry(adm_render_options_t* opts,
                                                         adm_speaker_geometry_t geometry) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(geometry) < ADM_SPEAKER_GEOMETRY_STANDARD ||
        static_cast<int>(geometry) > ADM_SPEAKER_GEOMETRY_APPLE) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.speaker_geometry = static_cast<mradm::SpeakerGeometry>(geometry);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_direct_speakers_routing_mode(adm_render_options_t* opts,
                                                                     adm_direct_speakers_routing_mode_t mode) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(mode) < ADM_DIRECT_SPEAKERS_ROUTING_AUTOMATIC ||
        static_cast<int>(mode) > ADM_DIRECT_SPEAKERS_ROUTING_MATRIX) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.direct_speakers_routing_mode = static_cast<mradm::DirectSpeakersRoutingMode>(mode);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_render_options_set_direct_speakers_matrix_path(adm_render_options_t* opts,
                                                                    const char* path) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (path == nullptr || path[0] == '\0') {
            opts->opts.direct_speakers_matrix_path = std::nullopt;
        } else {
            opts->opts.direct_speakers_matrix_path = std::filesystem::path{path};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_direct_speakers_matrix_json(adm_render_options_t* opts,
                                                                    const char* json) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        if (json == nullptr || json[0] == '\0') {
            opts->opts.direct_speakers_matrix_json = std::nullopt;
        } else {
            opts->opts.direct_speakers_matrix_json = std::string{json};
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_options_set_lfe_routing_mode(adm_render_options_t* opts,
                                                         adm_lfe_routing_mode_t mode) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    if (static_cast<int>(mode) < ADM_LFE_ROUTING_DIRECT || static_cast<int>(mode) > ADM_LFE_ROUTING_SPLIT_POWER) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.lfe_routing_mode = static_cast<mradm::LfeRoutingMode>(mode);
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

adm_error_code_t adm_render_options_set_iamf_layers(adm_render_options_t* opts, const char* iamf_layers_csv) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    try {
        opts->opts.iamf_layers =
            iamf_layers_csv == nullptr ? std::vector<std::string>{} : split_csv_list(iamf_layers_csv);
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
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

adm_error_code_t adm_render_options_set_final_gain_db(adm_render_options_t* opts, double db) noexcept {
    if (opts == nullptr) {
        return ADM_ERROR_OK;
    }
    // Unconstrained by design: no range limit, but reject non-finite values.
    if (!std::isfinite(db)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    opts->opts.final_gain_db = db;
    return ADM_ERROR_OK;
}

/* ── Cancellation (v1.4) ──────────────────────────────────────────────────── */

adm_cancel_token_t* adm_create_cancel_token(void) noexcept {
    try {
        return new adm_cancel_token_t{};
    } catch (...) {
        return nullptr;
    }
}

void adm_destroy_cancel_token(adm_cancel_token_t* token) noexcept {
    delete token;
}

void adm_cancel(adm_cancel_token_t* token) noexcept {
    if (token != nullptr) {
        // std::stop_source::request_stop is thread-safe and idempotent.
        token->source.request_stop();
    }
}

void adm_reset_cancel_token(adm_cancel_token_t* token) noexcept {
    if (token != nullptr) {
        // A stop_source cannot be un-requested; swap in a fresh one. Any render in
        // progress already holds its own stop_token copy, so this only affects the
        // next render that resolves a token from this source.
        token->source = std::stop_source{};
    }
}

void adm_render_options_set_cancel_token(adm_render_options_t* opts, adm_cancel_token_t* token) noexcept {
    if (opts != nullptr) {
        opts->cancel_token = token;
    }
}

/* ── Render ──────────────────────────────────────────────────────────────── */

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static adm_error_code_t render_file_impl(adm_context_t* context,
                                         const char* input_path,
                                         const char* output_path,
                                         const adm_render_options_t* opts,
                                         mradm::ProgressSink& progress_sink,
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
            if (opts->cancel_token != nullptr) {
                request.options.cancel_token = opts->cancel_token->source.get_token();
            }
        }

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
        auto* c_result = make_c_result(std::move(cpp_result), std::move(captured));
        if (c_result == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        *result = c_result;
        return code;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_render_file_ex(adm_context_t* context,
                                    const char* input_path,
                                    const char* output_path,
                                    const adm_render_options_t* opts,
                                    adm_progress_cb progress,
                                    void* user_data,
                                    adm_render_result_t** result) noexcept {
    CallbackProgressSink progress_sink(progress, user_data);
    return render_file_impl(context, input_path, output_path, opts, progress_sink, result);
}

adm_error_code_t adm_render_file_ex2(adm_context_t* context,
                                     const char* input_path,
                                     const char* output_path,
                                     const adm_render_options_t* opts,
                                     adm_progress_v2_cb progress,
                                     void* user_data,
                                     adm_render_result_t** result) noexcept {
    CallbackProgressSinkV2 progress_sink(progress, user_data);
    return render_file_impl(context, input_path, output_path, opts, progress_sink, result);
}

adm_error_code_t adm_render_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 adm_progress_cb progress,
                                 void* user_data,
                                 adm_render_result_t** result) noexcept {
    return adm_render_file_ex(context, input_path, output_path, nullptr, progress, user_data, result);
}

/* ── Preview session (v1.8) ──────────────────────────────────────────────── */

// cppcheck-suppress constParameterPointer -- context is non-const by the stable C ABI signature
adm_error_code_t adm_create_preview_session(adm_context_t* context,
                                            const char* input_path,
                                            const adm_render_options_t* opts,
                                            adm_preview_session_t** out) noexcept {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0' || out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(context);
        mradm::RenderOptions options;
        if (opts != nullptr) {
            options = opts->opts;
            if (opts->cancel_token != nullptr) {
                options.cancel_token = opts->cancel_token->source.get_token();
            }
        }
        mradm::NullLogSink null_sink;
        auto created = mradm::PreviewSession::create(input_path, std::move(options), null_sink);
        if (!created) {
            store_last_error(context, created.error());
            return map_error(created.error().code);
        }
        // cppcheck-suppress unreadVariable -- session is used (*out = session)
        auto* session = new (std::nothrow) adm_preview_session_t{std::move(*created)};
        if (session == nullptr) {
            context->last_error_message = "failed to allocate preview session handle";
            return ADM_ERROR_INTERNAL;
        }
        *out = session;
        return ADM_ERROR_OK;
    } catch (...) {
        if (context != nullptr) {
            context->last_error_message = "unexpected exception while creating preview session";
        }
        return ADM_ERROR_INTERNAL;
    }
}

void adm_destroy_preview_session(adm_preview_session_t* session) noexcept {
    delete session;
}

// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static adm_error_code_t preview_render_window_impl(adm_preview_session_t* session,
                                                   double start_sec,
                                                   double end_sec,
                                                   const char* output_path,
                                                   mradm::ProgressSink& progress_sink,
                                                   adm_render_result_t** result) noexcept {
    if (result != nullptr) {
        *result = nullptr;
    }
    if (session == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(start_sec) || start_sec < 0.0 || !std::isfinite(end_sec)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        // end_sec <= 0 means "to the end" (nullopt), matching set_render_end_sec.
        const std::optional<double> end = (end_sec > 0.0) ? std::optional<double>{end_sec} : std::nullopt;
        std::optional<std::filesystem::path> out_path;
        if (output_path != nullptr && output_path[0] != '\0') {
            out_path = std::filesystem::path{output_path};
        }

        std::vector<CLogEntry> captured;
        CollectingLogSink collecting(captured);
        mradm::NullLogSink null_sink;
        mradm::LogSink& log_sink =
            (result != nullptr) ? static_cast<mradm::LogSink&>(collecting) : static_cast<mradm::LogSink&>(null_sink);
        mradm::RenderResult cpp_result =
            session->session.render_window(start_sec, end, std::move(out_path), progress_sink, log_sink);

        const adm_error_code_t code = map_error(cpp_result.error.code);
        if (result == nullptr) {
            return code;
        }
        auto* c_result = make_c_result(std::move(cpp_result), std::move(captured));
        if (c_result == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        *result = c_result;
        return code;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_preview_render_window(adm_preview_session_t* session,
                                           double start_sec,
                                           double end_sec,
                                           const char* output_path,
                                           adm_progress_cb progress,
                                           void* user_data,
                                           adm_render_result_t** result) noexcept {
    CallbackProgressSink progress_sink(progress, user_data);
    return preview_render_window_impl(session, start_sec, end_sec, output_path, progress_sink, result);
}

adm_error_code_t adm_preview_render_window_v2(adm_preview_session_t* session,
                                              double start_sec,
                                              double end_sec,
                                              const char* output_path,
                                              adm_progress_v2_cb progress,
                                              void* user_data,
                                              adm_render_result_t** result) noexcept {
    CallbackProgressSinkV2 progress_sink(progress, user_data);
    return preview_render_window_impl(session, start_sec, end_sec, output_path, progress_sink, result);
}

/* ── Realtime monitor (v1.15) ──────────────────────────────────────────────── */

// cppcheck-suppress constParameterPointer ; context is part of the fixed C ABI signature.
adm_error_code_t adm_create_monitor_ex(adm_context_t* context,
                                       const char* input_path,
                                       const adm_render_options_t* opts,
                                       const char* device_id,
                                       adm_monitor_t** out) noexcept {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0' || out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(context);
        mradm::RenderOptions options;
        if (opts != nullptr) {
            options = opts->opts;
            if (opts->cancel_token != nullptr) {
                options.cancel_token = opts->cancel_token->source.get_token();
            }
        }
        const std::string device = device_id != nullptr ? std::string{device_id} : std::string{};
        auto created = mradm::MonitorSession::create(input_path, options, device);
        if (!created) {
            store_last_error(context, created.error());
            return map_error(created.error().code);
        }
        // cppcheck-suppress unreadVariable -- monitor is used (*out = monitor)
        auto* monitor = new (std::nothrow) adm_monitor_t{std::move(*created), {}, {}, {}};
        if (monitor == nullptr) {
            context->last_error_message = "failed to allocate realtime monitor handle";
            return ADM_ERROR_INTERNAL;
        }
        *out = monitor;
        return ADM_ERROR_OK;
    } catch (...) {
        if (context != nullptr) {
            context->last_error_message = "unexpected exception while creating realtime monitor";
        }
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_create_monitor(adm_context_t* context,
                                    const char* input_path,
                                    const adm_render_options_t* opts,
                                    adm_monitor_t** out) noexcept {
    return adm_create_monitor_ex(context, input_path, opts, nullptr, out);
}

// cppcheck-suppress constParameterPointer ; context is part of the fixed C ABI signature.
adm_error_code_t adm_monitor_output_devices_json(adm_context_t* context, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        // Minimal JSON string escape (", \, and control chars) — device names are arbitrary.
        auto escape = [](const std::string& s) {
            std::string e;
            e.reserve(s.size() + 2);
            for (const char c : s) {
                switch (c) {
                case '"':
                    e += R"(\")";
                    break;
                case '\\':
                    e += R"(\\)";
                    break;
                case '\n':
                    e += "\\n";
                    break;
                case '\r':
                    e += "\\r";
                    break;
                case '\t':
                    e += "\\t";
                    break;
                default:
                    e += c;
                    break;
                }
            }
            return e;
        };

        std::string json = "[";
        bool first = true;
        for (const auto& d : mradm::MonitorSession::list_output_devices()) {
            if (!first) {
                json += ',';
            }
            first = false;
            json += R"({"id":")" + escape(d.id) + R"(","name":")" + escape(d.name) + R"(","default":)" +
                    (d.is_default ? "true" : "false") + "}";
        }
        json += "]";

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

void adm_destroy_monitor(adm_monitor_t* monitor) noexcept {
    delete monitor;
}

const char* adm_monitor_last_error_message(const adm_monitor_t* monitor) noexcept {
    return monitor != nullptr ? monitor->last_error_message.c_str() : "";
}

adm_error_code_t adm_monitor_play(adm_monitor_t* monitor) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        monitor->session->play();
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while starting realtime monitor playback";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_pause(adm_monitor_t* monitor) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        monitor->session->pause();
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while pausing realtime monitor playback";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_seek(adm_monitor_t* monitor, double seconds) noexcept {
    if (monitor == nullptr || !monitor->session || !std::isfinite(seconds)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        auto result = monitor->session->seek_seconds(seconds);
        if (!result) {
            store_last_error(monitor, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while seeking realtime monitor";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_set_loop(adm_monitor_t* monitor, double start_seconds, double end_seconds) noexcept {
    if (monitor == nullptr || !monitor->session || !std::isfinite(start_seconds) || !std::isfinite(end_seconds)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        monitor->session->set_loop_seconds(start_seconds, end_seconds);
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while setting realtime monitor loop";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_set_overrides(adm_monitor_t* monitor,
                                           const adm_monitor_override_t* overrides,
                                           uint32_t count,
                                           uint64_t revision) noexcept {
    // overrides may be NULL only when clearing all overrides (count == 0).
    if (monitor == nullptr || !monitor->session || (overrides == nullptr && count != 0)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        mradm::LiveOverrides live;
        live.revision = revision;
        // The first element's struct_size is the array stride (all elements share it); it
        // must cover at least through divergence_scale so every field we read is present.
        // Reading via the stride lets callers built against a later (larger) struct pass an
        // array we still parse correctly.
        std::size_t stride = 0;
        if (count > 0) {
            stride = overrides[0].struct_size;
            constexpr std::size_t k_min = offsetof(adm_monitor_override_t, divergence_scale) + sizeof(float);
            if (stride < k_min) {
                return ADM_ERROR_INVALID_ARGUMENT;
            }
        }
        const auto* base = reinterpret_cast<const unsigned char*>(overrides);
        live.objects.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            adm_monitor_override_t src{};
            std::memcpy(&src, base + (static_cast<std::size_t>(i) * stride), std::min(stride, sizeof(src)));
            const auto has_field = [stride](std::size_t offset, std::size_t size) { return stride >= offset + size; };
            if (!has_field(offsetof(adm_monitor_override_t, extent_width_scale), sizeof(float))) {
                src.extent_width_scale = 1.0F;
            }
            if (!has_field(offsetof(adm_monitor_override_t, extent_height_scale), sizeof(float))) {
                src.extent_height_scale = 1.0F;
            }
            if (!has_field(offsetof(adm_monitor_override_t, extent_depth_scale), sizeof(float))) {
                src.extent_depth_scale = 1.0F;
            }
            if (!has_field(offsetof(adm_monitor_override_t, speaker_label), sizeof(const char*))) {
                src.speaker_label = nullptr; // legacy caller: whole-object override
            }
            if (!has_field(offsetof(adm_monitor_override_t, head_locked), sizeof(int32_t))) {
                src.head_locked = 0; // legacy caller: world-locked (participate in head tracking)
            }
            if (!has_field(offsetof(adm_monitor_override_t, mute), sizeof(int32_t))) {
                src.mute = 0; // legacy caller: audible
            }
            // Reject non-finite gain / scales: a NaN would otherwise poison the bus gain or
            // the topology rebuild downstream.
            if (!std::isfinite(src.gain_db) || !std::isfinite(src.diffuse_scale) || !std::isfinite(src.extent_scale) ||
                !std::isfinite(src.divergence_scale) || !std::isfinite(src.extent_width_scale) ||
                !std::isfinite(src.extent_height_scale) || !std::isfinite(src.extent_depth_scale)) {
                return ADM_ERROR_INVALID_ARGUMENT;
            }
            mradm::LiveObjectOverride ov;
            ov.object_id = src.object_id != nullptr ? std::string{src.object_id} : std::string{};
            ov.speaker_label = src.speaker_label != nullptr ? std::string{src.speaker_label} : std::string{};
            ov.gain_db = src.gain_db;
            ov.diffuse_scale = src.diffuse_scale;
            ov.extent_scale = src.extent_scale;
            ov.divergence_scale = src.divergence_scale;
            ov.extent_width_scale = src.extent_width_scale;
            ov.extent_height_scale = src.extent_height_scale;
            ov.extent_depth_scale = src.extent_depth_scale;
            if (!has_field(offsetof(adm_monitor_override_t, head_locked_valid), sizeof(int32_t)) ||
                src.head_locked_valid != 0) {
                // Missing valid field is an old caller: preserve v1.23 semantics,
                // where every override entry explicitly supplied head_locked.
                ov.head_locked = src.head_locked != 0;
            }
            ov.mute = src.mute != 0;
            live.objects.push_back(std::move(ov));
        }
        monitor->session->set_overrides(live);
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while applying realtime monitor overrides";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t
adm_monitor_set_listener_orientation(adm_monitor_t* monitor, float yaw_deg, float pitch_deg, float roll_deg) noexcept {
    if (monitor == nullptr || !monitor->session || !std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) ||
        !std::isfinite(roll_deg)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        mradm::ListenerOrientation orientation;
        orientation.yaw_deg = yaw_deg;
        orientation.pitch_deg = pitch_deg;
        orientation.roll_deg = roll_deg;
        monitor->session->set_listener_orientation(orientation);
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while setting realtime monitor listener orientation";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_switch_backend(adm_monitor_t* monitor, const adm_render_options_t* opts) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        mradm::RenderOptions options;
        if (opts != nullptr) {
            options = opts->opts;
        }
        auto result = monitor->session->switch_backend(options);
        if (!result) {
            store_last_error(monitor, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while switching realtime monitor backend";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_set_output_device(adm_monitor_t* monitor, const char* device_id) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        const std::string device = device_id != nullptr ? std::string{device_id} : std::string{};
        auto result = monitor->session->set_output_device(device);
        if (!result) {
            store_last_error(monitor, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        monitor->last_error_message = "unexpected exception while switching realtime monitor output device";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_get_status(adm_monitor_t* monitor, adm_monitor_status_t* out) noexcept {
    if (monitor == nullptr || !monitor->session || out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const mradm::MonitorStatusSnapshot s = monitor->session->status();
        adm_monitor_status_t filled{};
        filled.struct_size = out->struct_size;
        filled.state = static_cast<int32_t>(s.state);
        filled.playhead_frames = s.playhead_frames;
        filled.underruns = s.underruns;
        filled.buffered_frames = s.buffered_frames;
        filled.ring_fill = s.ring_fill;
        filled.ended = s.ended ? 1 : 0;
        filled.failed = s.failed ? 1 : 0;
        filled.override_revision = s.override_revision;
        // struct_size forward-compat: copy only the bytes the caller's struct holds.
        const std::size_t copy = std::min<std::size_t>(out->struct_size, sizeof(filled));
        std::memcpy(out, &filled, copy);
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_get_levels(adm_monitor_t* monitor, adm_monitor_levels_t* out) noexcept {
    if (monitor == nullptr || !monitor->session || out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const mradm::MonitorLevelsSnapshot l = monitor->session->levels();
        out->out_count = l.channels;
        const uint32_t n = std::min(out->capacity, l.channels);
        for (uint32_t c = 0; c < n; ++c) {
            if (out->peak != nullptr) {
                out->peak[c] = l.peak.at(c);
            }
            if (out->rms != nullptr) {
                out->rms[c] = l.rms.at(c);
            }
        }
        // v1.18 LUFS: only write fields the caller's struct_size actually reserves, so a
        // pre-v1.18 caller (smaller struct) is never written past its end.
        if (out->struct_size >= offsetof(adm_monitor_levels_t, integrated_lufs) + sizeof(float)) {
            out->momentary_lufs = l.momentary_lufs;
            out->shortterm_lufs = l.shortterm_lufs;
            out->integrated_lufs = l.integrated_lufs;
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

uint32_t adm_monitor_log_count(adm_monitor_t* monitor) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return 0;
    }
    try {
        return static_cast<uint32_t>(monitor->session->log_count());
    } catch (...) {
        return 0;
    }
}

int adm_monitor_log_entry(adm_monitor_t* monitor,
                          uint32_t index,
                          int32_t* out_level,
                          const char** out_module,
                          const char** out_message) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return 0;
    }
    try {
        mradm::LogLevel level{};
        if (!monitor->session->log_entry(index, level, monitor->log_module, monitor->log_message)) {
            return 0;
        }
        if (out_level != nullptr) {
            *out_level = static_cast<int32_t>(level);
        }
        if (out_module != nullptr) {
            *out_module = monitor->log_module.c_str();
        }
        if (out_message != nullptr) {
            *out_message = monitor->log_message.c_str();
        }
        return 1;
    } catch (...) {
        return 0;
    }
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

const char* adm_render_result_semantic_report_json(const adm_render_result_t* result) noexcept {
    if (result == nullptr || !result->semantic_report_json.has_value()) {
        return nullptr;
    }
    return result->semantic_report_json->c_str();
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
        clear_last_error(context);
        auto probe_result = context->service.probe(input_path);
        if (!probe_result) {
            store_last_error(context, probe_result.error());
            return map_error(probe_result.error().code);
        }

        if (out == nullptr) {
            return ADM_ERROR_OK;
        }

        // cppcheck-suppress unreadVariable -- info is read via *out = info below
        auto* info = new (std::nothrow) adm_scene_info_t{};
        if (info == nullptr) {
            context->last_error_message = "failed to allocate scene info handle";
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
        if (context != nullptr) {
            context->last_error_message = "unexpected exception while probing ADM file";
        }
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
        clear_last_error(context);
        auto json_result = context->service.inspect_json(input_path);
        if (!json_result) {
            store_last_error(context, json_result.error());
            return map_error(json_result.error().code);
        }
        if (out_json == nullptr) {
            return ADM_ERROR_OK; // validate-only: parsed successfully, allocate nothing
        }
        const std::string& json = *json_result;
        auto* buffer = new (std::nothrow) char[json.size() + 1];
        if (buffer == nullptr) {
            context->last_error_message = "failed to allocate ADM inspect JSON";
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, json.c_str(), json.size() + 1);
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        context->last_error_message = "unexpected exception while inspecting ADM file";
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
        clear_last_error(context);
        auto xml_result = context->service.axml(input_path);
        if (!xml_result) {
            store_last_error(context, xml_result.error());
            return map_error(xml_result.error().code);
        }
        if (out_xml == nullptr) {
            return ADM_ERROR_OK; // validate-only: chunk readable, allocate nothing
        }
        const std::string& xml = *xml_result;
        auto* buffer = new (std::nothrow) char[xml.size() + 1];
        if (buffer == nullptr) {
            context->last_error_message = "failed to allocate ADM XML";
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, xml.c_str(), xml.size() + 1);
        *out_xml = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        context->last_error_message = "unexpected exception while reading ADM XML";
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
        clear_last_error(context);
        auto tmpl_result = context->service.policy_template_json(input_path);
        if (!tmpl_result) {
            store_last_error(context, tmpl_result.error());
            return map_error(tmpl_result.error().code);
        }
        if (out_json == nullptr) {
            return ADM_ERROR_OK; // validate-only: scene parsed, allocate nothing
        }
        const std::string& tmpl = *tmpl_result;
        auto* buffer = new (std::nothrow) char[tmpl.size() + 1];
        if (buffer == nullptr) {
            context->last_error_message = "failed to allocate semantic-policy template JSON";
            return ADM_ERROR_INTERNAL;
        }
        std::char_traits<char>::copy(buffer, tmpl.c_str(), tmpl.size() + 1);
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        context->last_error_message = "unexpected exception while building semantic-policy template";
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_export_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 const adm_render_options_t* opts) noexcept {
    if (context == nullptr || input_path == nullptr || input_path[0] == '\0' || output_path == nullptr ||
        output_path[0] == '\0') {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        clear_last_error(context);
        mradm::NullLogSink logs;
        const mradm::RenderOptions default_options;
        const mradm::RenderOptions& options = (opts != nullptr) ? opts->opts : default_options;
        auto result = context->service.export_file(input_path, output_path, options, logs);
        if (!result) {
            store_last_error(context, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        context->last_error_message = "unexpected exception while exporting ADM file";
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

adm_error_code_t adm_input_layouts_json(adm_context_t* context, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        const std::string json = context->service.input_layouts_json();
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

adm_error_code_t adm_output_formats_json(adm_context_t* context, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        const std::string json = context->service.output_formats_json();
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

adm_error_code_t adm_render_support_matrix_json(adm_context_t* context, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (context == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }

    try {
        const std::string json = context->service.render_support_matrix_json();
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

/* ── Producer-neutral realtime Scene stream (v1.35) ─────────────────────── */

adm_error_code_t adm_create_scene_stream(adm_context_t* context,
                                         const adm_scene_stream_config_t* config,
                                         adm_scene_stream_t** out) noexcept {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (context == nullptr || config == nullptr || out == nullptr || config->struct_size < sizeof(*config)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(context);
        adm_scene_stream_config_t input{};
        std::memcpy(&input, config, sizeof(input));
        if ((input.renderer != ADM_RENDERER_SAF && input.renderer != ADM_RENDERER_SAF_BINAURAL) ||
            input.speaker_geometry < ADM_SPEAKER_GEOMETRY_STANDARD ||
            input.speaker_geometry > ADM_SPEAKER_GEOMETRY_APPLE ||
            input.speaker_spread_mode < ADM_SPEAKER_SPREAD_AUTOMATIC ||
            input.speaker_spread_mode > ADM_SPEAKER_SPREAD_MDAP ||
            input.binaural_spread_mode < ADM_BINAURAL_SPREAD_AUTOMATIC ||
            input.binaural_spread_mode > ADM_BINAURAL_SPREAD_SAF_SPREADER ||
            input.lfe_routing_mode < ADM_LFE_ROUTING_DIRECT || input.lfe_routing_mode > ADM_LFE_ROUTING_SPLIT_POWER) {
            context->last_error_message = "invalid live Scene renderer configuration enum";
            return ADM_ERROR_INVALID_ARGUMENT;
        }
        if (input.renderer == ADM_RENDERER_SAF && (input.output_layout == nullptr || input.output_layout[0] == '\0')) {
            context->last_error_message = "SAF VBAP live Scene rendering requires an output layout";
            return ADM_ERROR_INVALID_ARGUMENT;
        }
        if (input.renderer == ADM_RENDERER_SAF_BINAURAL && input.output_layout != nullptr &&
            input.output_layout[0] != '\0' && std::string_view{input.output_layout} != "binaural") {
            context->last_error_message = "SAF binaural live Scene output layout must be 'binaural'";
            return ADM_ERROR_UNSUPPORTED;
        }

        mradm::realtime::SceneStreamConfig converted;
        converted.renderer.renderer = static_cast<mradm::RendererSelection>(input.renderer);
        converted.renderer.output_layout = input.output_layout != nullptr ? input.output_layout : "binaural";
        if (input.sofa_path != nullptr && input.sofa_path[0] != '\0') {
            converted.renderer.sofa_path = input.sofa_path;
        }
        converted.renderer.speaker_geometry = static_cast<mradm::SpeakerGeometry>(input.speaker_geometry);
        converted.renderer.speaker_spread_mode = static_cast<mradm::SpeakerSpreadMode>(input.speaker_spread_mode);
        converted.renderer.binaural_spread_mode = static_cast<mradm::BinauralSpreadMode>(input.binaural_spread_mode);
        converted.renderer.lfe_routing_mode = static_cast<mradm::LfeRoutingMode>(input.lfe_routing_mode);
        converted.renderer.sample_rate = input.input_sample_rate;
        converted.output_sample_rate = input.output_sample_rate;
        converted.input_queue_samples = input.input_queue_samples;
        converted.input_queue_bytes = input.input_queue_bytes;
        converted.output_ring_frames = input.output_ring_frames;
        converted.startup_watermark_frames = input.startup_watermark_frames;

        auto created = mradm::realtime::SceneStreamEngine::create(std::move(converted));
        if (!created) {
            store_last_error(context, created.error());
            return map_error(created.error().code);
        }
        auto handle = std::make_unique<adm_scene_stream_t>();
        handle->engine = std::move(*created);
        *out = handle.release();
        return ADM_ERROR_OK;
    } catch (const std::exception& exception) {
        context->last_error_message =
            std::string{"unexpected exception while creating live Scene stream: "} + exception.what();
        return ADM_ERROR_INTERNAL;
    } catch (...) {
        context->last_error_message = "unexpected exception while creating live Scene stream";
        return ADM_ERROR_INTERNAL;
    }
}

void adm_destroy_scene_stream(adm_scene_stream_t* stream) noexcept {
    delete stream;
}

const char* adm_scene_stream_last_error_message(const adm_scene_stream_t* stream) noexcept {
    if (stream == nullptr) {
        return "";
    }
    const std::lock_guard<std::mutex> lock(stream->message_mutex);
    return stream->last_error_message.c_str();
}

adm_error_code_t adm_scene_stream_get_output_format(adm_scene_stream_t* stream,
                                                    adm_scene_output_format_t* out) noexcept {
    if (stream == nullptr || !stream->engine || !output_struct_valid(out)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    clear_last_error(stream);
    const auto format = stream->engine->output_format();
    adm_scene_output_format_t converted{};
    converted.sample_format = ADM_SCENE_SAMPLE_F32;
    converted.sample_rate = format.sample_rate;
    converted.channels = format.channels;
    converted.interleaved = 1;
    write_sized_output(out, converted);
    return ADM_ERROR_OK;
}

adm_error_code_t
adm_scene_stream_begin_epoch(adm_scene_stream_t* stream, uint64_t epoch_id, int64_t target_sample) noexcept {
    if (stream == nullptr || !stream->engine) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        auto result = stream->engine->begin_epoch(epoch_id, target_sample);
        if (!result) {
            store_last_error(stream, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while beginning live Scene epoch");
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_scene_stream_configure_generation(adm_scene_stream_t* stream,
                                                       uint64_t epoch_id,
                                                       uint64_t generation_id,
                                                       const adm_scene_element_descriptor_t* elements,
                                                       uint32_t element_count) noexcept {
    if (stream == nullptr || !stream->engine) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        std::vector<adm_scene_element_descriptor_t> borrowed;
        if (!load_sized_array(elements, element_count, sizeof(adm_scene_element_descriptor_t), borrowed)) {
            store_last_error_message(stream, "invalid live Scene element descriptor array/stride");
            return ADM_ERROR_INVALID_ARGUMENT;
        }
        std::vector<mradm::live_scene::ElementDescriptor> converted;
        converted.reserve(borrowed.size());
        for (const auto& descriptor : borrowed) {
            if (descriptor.role < ADM_SCENE_ELEMENT_OBJECT || descriptor.role > ADM_SCENE_ELEMENT_LFE) {
                store_last_error_message(stream, "invalid live Scene element role");
                return ADM_ERROR_INVALID_ARGUMENT;
            }
            mradm::live_scene::ElementDescriptor element;
            element.element_id = descriptor.element_id;
            element.role = static_cast<mradm::live_scene::ElementRole>(descriptor.role);
            if (descriptor.speaker_label != nullptr) {
                element.speaker_label = descriptor.speaker_label;
            }
            element.flags = descriptor.flags;
            element.has_position = descriptor.has_position != 0;
            element.x = descriptor.position_x;
            element.y = descriptor.position_y;
            element.z = descriptor.position_z;
            converted.push_back(std::move(element));
        }
        auto result = stream->engine->configure_generation(epoch_id, generation_id, converted);
        if (!result) {
            store_last_error(stream, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while configuring live Scene generation");
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_scene_stream_submit_frame(adm_scene_stream_t* stream,
                                               const adm_scene_frame_t* frame,
                                               uint32_t timeout_ms,
                                               int32_t* out_submit_status) noexcept {
    if (out_submit_status != nullptr) {
        *out_submit_status = ADM_SCENE_SUBMIT_CLOSED;
    }
    if (stream == nullptr || !stream->engine || frame == nullptr || out_submit_status == nullptr ||
        frame->struct_size < sizeof(*frame)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        adm_scene_frame_t input{};
        std::memcpy(&input, frame, sizeof(input));
        std::vector<adm_scene_pcm_plane_t> pcm;
        std::vector<adm_scene_initial_state_t> initial_states;
        std::vector<adm_scene_metadata_update_t> updates;
        if (!load_sized_array(input.pcm, input.pcm_count, sizeof(adm_scene_pcm_plane_t), pcm) ||
            !load_sized_array(
                input.initial_states, input.initial_state_count, sizeof(adm_scene_initial_state_t), initial_states) ||
            !load_sized_array(
                input.metadata_updates, input.metadata_update_count, sizeof(adm_scene_metadata_update_t), updates)) {
            store_last_error_message(stream, "invalid live Scene frame array pointer/stride");
            return ADM_ERROR_INVALID_ARGUMENT;
        }

        std::vector<mradm::realtime::ScenePcmPlaneView> pcm_views;
        pcm_views.reserve(pcm.size());
        std::ranges::transform(pcm, std::back_inserter(pcm_views), [](const adm_scene_pcm_plane_t& plane) {
            return mradm::realtime::ScenePcmPlaneView{
                plane.element_id, plane.samples, plane.sample_count, plane.stride, plane.has_signal != 0};
        });
        std::vector<mradm::live_scene::StateEntry> state_views;
        state_views.reserve(initial_states.size());
        for (const auto& initial : initial_states) {
            if (initial.state.struct_size < sizeof(adm_scene_object_state_t)) {
                store_last_error_message(stream, "invalid live Scene initial-state struct_size");
                return ADM_ERROR_INVALID_ARGUMENT;
            }
            state_views.push_back({initial.element_id, to_scene_state(initial.state)});
        }
        std::vector<mradm::live_scene::MetadataUpdate> update_views;
        update_views.reserve(updates.size());
        for (const auto& update : updates) {
            if (update.state.struct_size < sizeof(adm_scene_object_state_t)) {
                store_last_error_message(stream, "invalid live Scene metadata target struct_size");
                return ADM_ERROR_INVALID_ARGUMENT;
            }
            update_views.push_back({update.element_id,
                                    update.offset_samples,
                                    update.ramp_duration_samples,
                                    update.changed_fields,
                                    to_scene_state(update.state),
                                    0U});
        }

        const mradm::realtime::SceneFrameView view{input.epoch_id,
                                                   input.generation_id,
                                                   input.media_sample_start,
                                                   input.duration_samples,
                                                   input.flags,
                                                   pcm_views,
                                                   state_views,
                                                   update_views};
        auto result = stream->engine->submit_frame(view, std::chrono::milliseconds{timeout_ms});
        if (!result) {
            store_last_error(stream, result.error());
            return map_error(result.error().code);
        }
        *out_submit_status = static_cast<int32_t>(*result);
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while submitting live Scene frame");
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t
adm_scene_stream_signal_end(adm_scene_stream_t* stream, uint64_t epoch_id, int64_t end_sample) noexcept {
    if (stream == nullptr || !stream->engine) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        auto result = stream->engine->signal_end(epoch_id, end_sample);
        if (!result) {
            store_last_error(stream, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while ending live Scene epoch");
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_scene_stream_pull(adm_scene_stream_t* stream,
                                       float* interleaved_output,
                                       uint32_t frames,
                                       adm_scene_pull_result_t* result) noexcept {
    if (stream == nullptr || !stream->engine || !output_struct_valid(result) ||
        (frames != 0U && interleaved_output == nullptr)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    const auto pulled = stream->engine->pull(interleaved_output, frames);
    adm_scene_pull_result_t converted{};
    converted.flags = pulled.flags;
    converted.epoch_id = pulled.epoch_id;
    converted.first_media_frame = pulled.first_media_frame;
    converted.media_frames = pulled.media_frames;
    converted.requested_frames = pulled.requested_frames;
    write_sized_output(result, converted);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_scene_stream_get_status(adm_scene_stream_t* stream, adm_scene_stream_status_t* out) noexcept {
    if (stream == nullptr || !stream->engine || !output_struct_valid(out)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    const auto status = stream->engine->status();
    adm_scene_stream_status_t converted{};
    converted.state = static_cast<int32_t>(status.state);
    converted.epoch_id = status.epoch_id;
    converted.generation_id = status.generation_id;
    converted.queued_input_samples = status.queued_input_samples;
    converted.queued_input_bytes = status.queued_input_bytes;
    converted.buffered_output_frames = status.buffered_output_frames;
    converted.media_frames_pulled = status.media_frames_pulled;
    converted.underruns = status.underruns;
    converted.semantic_degradations = status.semantic_degradations;
    converted.ring_fill = status.ring_fill;
    converted.ended = status.ended ? 1 : 0;
    converted.failed = status.failed ? 1 : 0;
    write_sized_output(out, converted);
    return ADM_ERROR_OK;
}

uint32_t adm_scene_stream_log_count(adm_scene_stream_t* stream) noexcept {
    if (stream == nullptr || !stream->engine) {
        return 0U;
    }
    try {
        const std::lock_guard<std::mutex> lock(stream->message_mutex);
        stream->diagnostic_message.clear();
        return static_cast<uint32_t>(
            std::min<std::size_t>(stream->engine->diagnostic_count(), std::numeric_limits<uint32_t>::max()));
    } catch (...) {
        return 0U;
    }
}

int adm_scene_stream_log_entry(adm_scene_stream_t* stream, uint32_t index, adm_scene_diagnostic_t* out) noexcept {
    if (stream == nullptr || !stream->engine || !output_struct_valid(out)) {
        return 0;
    }
    try {
        const std::lock_guard<std::mutex> lock(stream->message_mutex);
        stream->diagnostic_message.clear();
        const auto diagnostic = stream->engine->diagnostic(index);
        if (!diagnostic.has_value()) {
            return 0;
        }
        stream->diagnostic_message = diagnostic->message;
        adm_scene_diagnostic_t converted{};
        converted.level = to_c_log_level(diagnostic->level);
        converted.code = static_cast<int32_t>(diagnostic->code);
        converted.epoch_id = diagnostic->epoch_id;
        converted.generation_id = diagnostic->generation_id;
        converted.element_id = diagnostic->element_id;
        converted.field_mask = diagnostic->field_mask;
        converted.message = stream->diagnostic_message.c_str();
        write_sized_output(out, converted);
        return 1;
    } catch (...) {
        return 0;
    }
}
