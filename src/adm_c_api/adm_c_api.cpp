#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/c_api.h"
#include "adm/head_tracking.h"
#include "adm/hptf.h"
#include "adm/monitor.h"
#include "adm/render.h"
#include "adm/room.h"

#include "../adm_engine/scene_output_session.h"
#include "../adm_render_room/room_internal.h"
#include "capi_ffi.h"
#include "osc_ffi.h"
#include "scene_stream_engine.h"

namespace {

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
static_assert(static_cast<int>(mradm::RendererSelection::triple_balance) == ADM_RENDERER_TRIPLE_BALANCE);
static_assert(static_cast<int>(mradm::RendererSelection::room_binaural) == ADM_RENDERER_ROOM_BINAURAL);
static_assert(static_cast<int>(mradm::TripleBalanceMode::standard) == ADM_TRIPLE_BALANCE_STANDARD);
static_assert(static_cast<int>(mradm::TripleBalanceMode::d) == ADM_TRIPLE_BALANCE_D);

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

} // namespace

namespace {
static_assert(ADM_ROOM_QUALITY_EXACT == static_cast<int>(mradm::RoomQuality::exact));
static_assert(ADM_ROOM_QUALITY_REALTIME == static_cast<int>(mradm::RoomQuality::realtime));
static_assert(sizeof(adm_room_quality_t) == sizeof(std::uint32_t));

bool valid_room_quality(adm_room_quality_t quality) {
    return quality == ADM_ROOM_QUALITY_EXACT || quality == ADM_ROOM_QUALITY_REALTIME;
}

mradm::Result<mradm::RoomConfig> to_room_config(const adm_room_config_t* config) {
    if (config == nullptr || config->struct_size < sizeof(*config) || config->reserved != 0 ||
        config->room == nullptr || !std::isfinite(config->metres_per_unit) ||
        (config->metres_per_unit != 0 && config->metres_per_unit < 0.01) || config->metres_per_unit > 100) {
        return mradm::make_error(mradm::ErrorCode::invalid_argument, "无效的房间配置");
    }
    mradm::RoomConfig result;
    result.profile = mradm::room_internal::Access::adopt(mradm_capi_room_profile(config->room));
    if (!result.profile) {
        throw std::bad_alloc();
    }
    result.metres_per_unit = config->metres_per_unit;
    if (config->hrir_manifest != nullptr) {
        result.hrir_manifest =
            std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(config->hrir_manifest)}};
    }
    return result;
}
// A string the caller releases with adm_free_string (allocated by the Rust C ABI); NULL when out of memory.
char* c_string(std::string_view text) noexcept {
    return mradm_capi_string(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void owned_string(const std::string& text, char** out) {
    *out = c_string(text);
    if (*out == nullptr) {
        throw std::bad_alloc();
    }
}

std::string capi_text(MradmCapiText t) {
    return {reinterpret_cast<const char*>(t.data), t.len};
}

std::filesystem::path capi_path(MradmCapiText t) {
    return {std::u8string(reinterpret_cast<const char8_t*>(t.data), t.len)};
}

std::vector<std::string> capi_texts(const MradmCapiText* items, std::size_t len) {
    std::vector<std::string> out;
    for (const auto& item : std::span{items, len}) {
        out.push_back(capi_text(item));
    }
    return out;
}

// The texts, paths, room and screens of the Rust render options.
void capi_text_options(mradm::RenderOptions& o, const MradmCapiOptions& v) {
    o.output_layout = capi_text(v.output_layout);
    if (v.has_input_layout != 0U) {
        o.input_layout = capi_text(v.input_layout);
    }
    o.input_channel_labels = capi_texts(v.input_channel_labels, v.input_channel_labels_len);
    if (v.has_sofa != 0U) {
        o.sofa_path = capi_path(v.sofa);
    }
    if (v.has_policy_path != 0U) {
        o.semantic_policy_path = capi_path(v.policy_path);
    }
    if (v.has_policy_json != 0U) {
        o.semantic_policy_json = capi_text(v.policy_json);
    }
    if (v.has_matrix_path != 0U) {
        o.direct_speakers_matrix_path = capi_path(v.matrix_path);
    }
    if (v.has_matrix_json != 0U) {
        o.direct_speakers_matrix_json = capi_text(v.matrix_json);
    }
    if (v.has_report_path != 0U) {
        o.semantic_report_path = capi_path(v.report_path);
    }
    o.iamf_layers = capi_texts(v.iamf_layers, v.iamf_layers_len);
    o.room.hrir_manifest = capi_path(v.room_hrir);
    o.room.metres_per_unit = v.room_metres;
    o.room.quality = static_cast<mradm::RoomQuality>(v.room_quality);
    const auto screen = [](const MradmCapiScreen& s) {
        return mradm::ScreenGeometry{
            s.cartesian != 0U, s.aspect_ratio, {s.centre[0], s.centre[1], s.centre[2]}, s.width};
    };
    if (v.has_reference != 0U) {
        o.screens.reference = screen(v.reference);
    }
    if (v.has_reproduction != 0U) {
        o.screens.reproduction = screen(v.reproduction);
    }
}

// The Rust render options as the C++ RenderOptions the Monitor sessions take.
mradm::RenderOptions render_options(const adm_render_options_t* opts) {
    mradm::RenderOptions o;
    if (opts == nullptr) {
        return o;
    }
    MradmCapiOptions v{};
    const std::unique_ptr<MradmCapiOptionsLists, decltype(&mradm_capi_options_release)> lists(
        mradm_capi_options(opts, &v), &mradm_capi_options_release);
    if (!lists) {
        throw std::bad_alloc();
    }
    o.room.profile = mradm::room_internal::Access::adopt(v.room_profile);
    capi_text_options(o, v);
    o.renderer = static_cast<mradm::RendererSelection>(v.renderer);
    o.monitor_system_spatial = v.monitor_system_spatial != 0U;
    o.measure_loudness = v.measure_loudness != 0U;
    o.loudness_target_lufs = v.loudness_target_lufs;
    o.peak_limit = v.peak_limit != 0U;
    o.peak_limit_dbtp = v.peak_limit_dbtp;
    o.peak_normalize_to_limit = v.peak_normalize_to_limit != 0U;
    o.final_gain_db = v.final_gain_db;
    o.output_bit_depth = static_cast<mradm::OutputBitDepth>(v.output_bit_depth);
    o.opus_bitrate_per_ch_kbps = v.opus_bitrate_per_ch_kbps;
    o.apac_bitrate_kbps = v.apac_bitrate_kbps;
    o.apac_drc_music = v.apac_drc_music != 0U;
    o.apac_container =
        v.apac_caf != 0U ? mradm::RenderOptions::ApacContainer::caf : mradm::RenderOptions::ApacContainer::mpeg4;
    o.capture_semantic_report = v.capture_report != 0U;
    o.default_interp_ms = v.default_interp_ms;
    o.object_smoothing_frames = v.object_smoothing_frames;
    o.speaker_geometry = static_cast<mradm::SpeakerGeometry>(v.speaker_geometry);
    o.direct_speakers_routing_mode = static_cast<mradm::DirectSpeakersRoutingMode>(v.routing);
    o.speaker_spread_mode = static_cast<mradm::SpeakerSpreadMode>(v.speaker_spread);
    o.triple_balance_mode = static_cast<mradm::TripleBalanceMode>(v.triple_balance_mode);
    o.binaural_spread_mode = static_cast<mradm::BinauralSpreadMode>(v.binaural_spread);
    o.lfe_routing_mode = static_cast<mradm::LfeRoutingMode>(v.lfe_routing);
    o.apple_speaker_rendering_flags = v.apple_speaker_rendering_flags != 0U;
    o.render_start_sec = v.render_start_sec;
    if (v.has_end != 0U) {
        o.render_end_sec = v.render_end_sec;
    }
    o.iamf_container =
        v.iamf_mp4 != 0U ? mradm::RenderOptions::IamfContainer::mp4 : mradm::RenderOptions::IamfContainer::obu;
    return o;
}

bool set_last_error(adm_context_t* context, std::string_view message) noexcept {
    return mradm_capi_context_set_error(
               context, reinterpret_cast<const std::uint8_t*>(message.data()), message.size()) == ADM_ERROR_OK;
}

void clear_last_error(adm_context_t* context) noexcept {
    set_last_error(context, {});
}

void store_last_error(adm_context_t* context, const mradm::Error& error) {
    if (context == nullptr) {
        return;
    }
    std::string message = error.message;
    if (!error.context.empty()) {
        message += ": ";
        message += error.context;
    }
    if (!set_last_error(context, message)) {
        throw std::bad_alloc();
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

struct adm_osc_head_tracking_t {
    mradm::OscHeadTrackingReceiver receiver;
    std::string error_copy;
};

static_assert(static_cast<int>(mradm::OscHeadTrackingState::idle) == ADM_OSC_HEAD_TRACKING_IDLE);
static_assert(static_cast<int>(mradm::OscHeadTrackingState::waiting) == ADM_OSC_HEAD_TRACKING_WAITING);
static_assert(static_cast<int>(mradm::OscHeadTrackingState::active) == ADM_OSC_HEAD_TRACKING_ACTIVE);
static_assert(static_cast<int>(mradm::OscHeadTrackingState::stale) == ADM_OSC_HEAD_TRACKING_STALE);
static_assert(static_cast<int>(mradm::OscHeadTrackingState::stopped) == ADM_OSC_HEAD_TRACKING_STOPPED);
static_assert(static_cast<int>(mradm::OscHeadTrackingState::failed) == ADM_OSC_HEAD_TRACKING_FAILED);

struct adm_scene_stream_t {
    std::shared_ptr<mradm::realtime::SceneStreamEngine> engine;
    mutable std::mutex message_mutex;
    std::string last_error_message;
    // Backs adm_scene_diagnostic_t::message until another non-pull stream call.
    std::string diagnostic_message;
};

struct adm_scene_output_t {
    std::unique_ptr<mradm::realtime::SceneOutputSession> session;
    mutable std::mutex mutex;
    std::string message;
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
    converted.divergence_azimuth_range = state.divergence_azimuth_range;
    converted.divergence_position_range = state.divergence_position_range;
    if (state.has_channel_lock_max_distance != 0) {
        converted.channel_lock_max_distance = state.channel_lock_max_distance;
    }
    return converted;
}

[[nodiscard]] mradm::Result<mradm::live_scene::RendererConfig>
to_scene_renderer_config(const adm_scene_renderer_config_t* config, std::uint32_t sample_rate) {
    if (config == nullptr || config->struct_size < sizeof(*config)) {
        return mradm::make_error(mradm::ErrorCode::invalid_argument,
                                 "invalid live Scene renderer configuration struct_size");
    }
    adm_scene_renderer_config_t input{};
    std::memcpy(&input, config, sizeof(input));
    if ((input.renderer != ADM_RENDERER_SAF && input.renderer != ADM_RENDERER_SAF_BINAURAL &&
         input.renderer != ADM_RENDERER_TRIPLE_BALANCE && input.renderer != ADM_RENDERER_ROOM_BINAURAL) ||
        input.speaker_geometry < ADM_SPEAKER_GEOMETRY_STANDARD || input.speaker_geometry > ADM_SPEAKER_GEOMETRY_APPLE ||
        input.speaker_spread_mode < ADM_SPEAKER_SPREAD_AUTOMATIC ||
        input.speaker_spread_mode > ADM_SPEAKER_SPREAD_MDAP ||
        input.binaural_spread_mode < ADM_BINAURAL_SPREAD_AUTOMATIC ||
        input.binaural_spread_mode > ADM_BINAURAL_SPREAD_SAF_SPREADER ||
        input.lfe_routing_mode < ADM_LFE_ROUTING_DIRECT || input.lfe_routing_mode > ADM_LFE_ROUTING_SPLIT_POWER ||
        input.object_smoothing_frames > 48000U) {
        return mradm::make_error(mradm::ErrorCode::invalid_argument, "invalid live Scene renderer configuration value");
    }
    const bool binaural = input.renderer == ADM_RENDERER_SAF_BINAURAL || input.renderer == ADM_RENDERER_ROOM_BINAURAL;
    const std::string_view layout = input.output_layout != nullptr ? input.output_layout : "";
    if (!binaural && layout.empty()) {
        return mradm::make_error(mradm::ErrorCode::invalid_argument,
                                 "Speaker live Scene rendering requires an output layout");
    }
    if (!binaural && layout == "0+2+0") {
        return mradm::make_error(mradm::ErrorCode::unsupported,
                                 "two-channel loudspeaker rendering is unavailable; use binaural output instead");
    }
    if (!binaural && input.sofa_path != nullptr && input.sofa_path[0] != '\0') {
        return mradm::make_error(mradm::ErrorCode::unsupported,
                                 "a live Scene SOFA path is valid only for SAF binaural rendering");
    }
    if (binaural && !layout.empty() && layout != "binaural") {
        return mradm::make_error(mradm::ErrorCode::unsupported,
                                 "SAF binaural live Scene output layout must be 'binaural'");
    }

    mradm::live_scene::RendererConfig converted;
    converted.renderer = static_cast<mradm::RendererSelection>(input.renderer);
    converted.output_layout = layout.empty() ? "binaural" : layout;
    converted.sofa_path = input.sofa_path != nullptr ? input.sofa_path : "";
    converted.speaker_geometry = static_cast<mradm::SpeakerGeometry>(input.speaker_geometry);
    converted.speaker_spread_mode = static_cast<mradm::SpeakerSpreadMode>(input.speaker_spread_mode);
    converted.binaural_spread_mode = static_cast<mradm::BinauralSpreadMode>(input.binaural_spread_mode);
    converted.lfe_routing_mode = static_cast<mradm::LfeRoutingMode>(input.lfe_routing_mode);
    converted.object_smoothing_frames = input.object_smoothing_frames;
    converted.sample_rate = sample_rate;
    return converted;
}

[[nodiscard]] bool to_scene_semantic_identity(const adm_scene_semantic_identity_t* source,
                                              mradm::live_scene::SemanticIdentity& destination) {
    if (source == nullptr) {
        return true;
    }
    if (source->struct_size < sizeof(*source) ||
        (source->has_importance != 0 && (source->importance < 0 || source->importance > 10)) ||
        (source->has_dialogue_id != 0 && (source->dialogue_id < 0 || source->dialogue_id > 2))) {
        return false;
    }
    destination.object_id = source->object_id != nullptr ? source->object_id : "";
    destination.object_name = source->object_name != nullptr ? source->object_name : "";
    destination.track_uid = source->track_uid != nullptr ? source->track_uid : "";
    if (source->has_importance != 0) {
        destination.importance = source->importance;
    }
    if (source->has_dialogue_id != 0) {
        destination.dialogue_id = source->dialogue_id;
    }
    std::vector<adm_scene_semantic_entity_t> contents;
    std::vector<adm_scene_semantic_entity_t> programmes;
    if (!load_sized_array(source->contents, source->content_count, sizeof(adm_scene_semantic_entity_t), contents) ||
        !load_sized_array(
            source->programmes, source->programme_count, sizeof(adm_scene_semantic_entity_t), programmes)) {
        return false;
    }
    destination.contents.reserve(contents.size());
    for (const auto& entity : contents) {
        destination.contents.push_back(
            {entity.id != nullptr ? entity.id : "", entity.name != nullptr ? entity.name : ""});
    }
    destination.programmes.reserve(programmes.size());
    for (const auto& entity : programmes) {
        destination.programmes.push_back(
            {entity.id != nullptr ? entity.id : "", entity.name != nullptr ? entity.name : ""});
    }
    return true;
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

adm_head_tracking_pose_t head_tracking_pose(const mradm::OscHeadTrackingSnapshot& snapshot) noexcept {
    adm_head_tracking_pose_t result{};
    result.struct_size = sizeof(result);
    result.protocol_version = snapshot.timing.protocol_version;
    result.has_pose = snapshot.has_pose ? 1U : 0U;
    result.fresh = snapshot.fresh ? 1U : 0U;
    result.receiver_session_id = snapshot.session_id;
    result.receiver_sequence = snapshot.sequence;
    result.receiver_received_ns = snapshot.received_ns;
    result.age_ms = snapshot.age_ms;
    const auto& t = snapshot.timing;
    result.instance_id = t.instance_id;
    result.source_session_id = t.source_session_id;
    result.source_sequence = t.source_sequence;
    result.tx_sequence = t.tx_sequence;
    result.reference_epoch = t.reference_epoch;
    result.metadata_revision = t.metadata_revision;
    result.source_received_ns = t.source_received_ns;
    result.source_age_at_send_ns = t.source_age_at_send_ns;
    result.sample_time_kind = t.sample_time_kind;
    result.sample_time_ms = t.sample_time_ms;
    result.sample_clock_epoch = t.sample_clock_epoch;
    std::ranges::copy(snapshot.orientation.quaternion_xyzw, std::begin(result.quaternion_xyzw));
    result.yaw_deg = snapshot.orientation.euler_deg[0];
    result.pitch_deg = snapshot.orientation.euler_deg[1];
    result.roll_deg = snapshot.orientation.euler_deg[2];
    return result;
}

} // namespace

// The caller's progress callbacks as the Rust render calls them (mradm-capi `render`): an exception a
// C++ caller throws from one ends that render with ADM_ERROR_INTERNAL instead of unwinding into Rust.
extern "C" uint32_t mradm_capi_progress_v1(
    adm_progress_cb callback, double fraction, const char* stage, const char* message, void* user) noexcept {
    try {
        callback(fraction, stage, message, user);
        return 0U;
    } catch (...) {
        return 1U;
    }
}

extern "C" uint32_t
mradm_capi_progress_v2(adm_progress_v2_cb callback, const adm_progress_event_v2_t* event, void* user) noexcept {
    try {
        callback(event, user);
        return 0U;
    } catch (...) {
        return 1U;
    }
}

/* ── Version ──────────────────────────────────────────────────────────────── */

adm_error_code_t adm_create_osc_head_tracking(const adm_osc_head_tracking_config_t* config,
                                              adm_osc_head_tracking_t** out) noexcept {
    if (out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (config != nullptr &&
        (config->struct_size < sizeof(adm_osc_head_tracking_config_t) || config->listen_port > 65535U)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    if (config != nullptr && config->source_id != nullptr && !std::string_view{config->source_id}.empty() &&
        mradm_osc_valid_source_id(reinterpret_cast<const std::uint8_t*>(config->source_id),
                                  std::string_view{config->source_id}.size()) == 0U) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        *out = new adm_osc_head_tracking_t{
            mradm::OscHeadTrackingReceiver{static_cast<std::uint16_t>(config != nullptr ? config->listen_port : 9000U),
                                           config != nullptr && config->source_id != nullptr ? config->source_id : ""},
            {}};
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

void adm_destroy_osc_head_tracking(adm_osc_head_tracking_t* receiver) noexcept {
    delete receiver;
}

adm_error_code_t adm_osc_head_tracking_start(adm_osc_head_tracking_t* receiver) noexcept {
    if (receiver == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto result = receiver->receiver.start();
        return result ? ADM_ERROR_OK : map_error(result.error().code);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

void adm_osc_head_tracking_stop(adm_osc_head_tracking_t* receiver) noexcept {
    if (receiver != nullptr) {
        receiver->receiver.stop();
    }
}

adm_error_code_t adm_osc_head_tracking_get_pose(const adm_osc_head_tracking_t* receiver,
                                                adm_head_tracking_pose_t* out) noexcept {
    if (receiver == nullptr || out == nullptr || out->struct_size < sizeof(adm_head_tracking_pose_t)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto snapshot = receiver->receiver.snapshot();
        const auto result = head_tracking_pose(snapshot);
        std::memcpy(out, &result, sizeof(result));
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_osc_head_tracking_get_status(const adm_osc_head_tracking_t* receiver,
                                                  adm_osc_head_tracking_status_t* out) noexcept {
    if (receiver == nullptr || out == nullptr || out->struct_size < sizeof(adm_osc_head_tracking_status_t)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto snapshot = receiver->receiver.snapshot();
        adm_osc_head_tracking_status_t result{};
        result.struct_size = sizeof(result);
        result.state = static_cast<std::int32_t>(snapshot.state);
        result.bound_port = snapshot.bound_port;
        result.has_pose = snapshot.has_pose ? 1U : 0U;
        result.session_id = snapshot.session_id;
        result.sequence = snapshot.sequence;
        result.packets_received = snapshot.packets_received;
        result.rejected_packets = snapshot.rejected_packets;
        result.recovery_count = snapshot.recovery_count;
        result.age_ms = snapshot.age_ms;
        result.pose_packets = snapshot.pose_packets;
        result.telemetry_packets = snapshot.telemetry_packets;
        result.ignored_sources = snapshot.ignored_sources;
        result.protocol_mismatches = snapshot.protocol_mismatches;
        result.missing_tx_packets = snapshot.missing_tx_packets;
        result.has_heartbeat = snapshot.has_heartbeat ? 1U : 0U;
        result.heartbeat_alive = snapshot.heartbeat_alive ? 1U : 0U;
        result.heartbeat_age_ms = snapshot.heartbeat_age_ms;
        std::memcpy(out, &result, sizeof(result));
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_osc_head_tracking_snapshot_json(const adm_osc_head_tracking_t* receiver,
                                                     char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (receiver == nullptr || out_json == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto text = receiver->receiver.snapshot_json();
        auto* buffer = c_string(text);
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
        *out_json = buffer;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

const char* adm_osc_head_tracking_last_error_message(adm_osc_head_tracking_t* receiver) noexcept {
    if (receiver == nullptr) {
        return "";
    }
    try {
        receiver->error_copy = receiver->receiver.last_error();
        return receiver->error_copy.c_str();
    } catch (...) {
        return "OSC 错误信息查询失败";
    }
}

/* ── Progress stage (v1.7) ──────────────────────────────────────────────────── */

/* ── Options builder ─────────────────────────────────────────────────────── */

/* ── Cancellation (v1.4) ──────────────────────────────────────────────────── */

/* ── Render ──────────────────────────────────────────────────────────────── */

/* ── Preview session (v1.8) ──────────────────────────────────────────────── */

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
        const auto options = render_options(opts);
        const std::string device = device_id != nullptr ? std::string{device_id} : std::string{};
        auto created = mradm::MonitorSession::create(input_path, options, device);
        if (!created) {
            store_last_error(context, created.error());
            return map_error(created.error().code);
        }
        // cppcheck-suppress unreadVariable -- monitor is used (*out = monitor)
        auto* monitor = new (std::nothrow) adm_monitor_t{std::move(*created), {}, {}, {}};
        if (monitor == nullptr) {
            set_last_error(context, "failed to allocate realtime monitor handle");
            return ADM_ERROR_INTERNAL;
        }
        *out = monitor;
        return ADM_ERROR_OK;
    } catch (...) {
        set_last_error(context, "unexpected exception while creating realtime monitor");
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

        auto* buffer = c_string(json);
        if (buffer == nullptr) {
            return ADM_ERROR_INTERNAL;
        }
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
        auto applied = monitor->session->set_overrides(live);
        if (!applied) {
            monitor->last_error_message = applied.error().message + ": " + applied.error().context;
            return static_cast<adm_error_code_t>(applied.error().code);
        }
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
        auto result = monitor->session->switch_backend(render_options(opts));
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

namespace {
[[nodiscard]] bool hptf_output_stride(const adm_hptf_band_t* output, uint32_t count, uint32_t& stride) {
    if (count == 0U) {
        return true;
    }
    if (output == nullptr) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(output);
    std::memcpy(&stride, bytes, sizeof(stride));
    if (stride < offsetof(adm_hptf_band_t, q) + sizeof(double) ||
        static_cast<std::size_t>(count) > std::numeric_limits<std::size_t>::max() / stride) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t size = 0;
        std::memcpy(&size, bytes + (static_cast<std::size_t>(index) * stride), sizeof(size));
        if (size != stride) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] adm_hptf_info_t to_hptf_info(const mradm::HptfInfo& info) {
    return {sizeof(adm_hptf_info_t),
            info.enabled ? 1 : 0,
            info.band_count,
            info.sample_rate,
            info.preamp_db,
            info.auto_trim_db,
            info.max_response_db,
            0,
            info.applied_revision};
}
template <typename T> mradm::Result<void> write_hptf_info(T* out, const mradm::HptfStereoInfo& info) {
    if constexpr (std::is_same_v<T, adm_hptf_info_t>) {
        if (!info.symmetric) {
            return mradm::make_error(mradm::ErrorCode::unsupported, "左右 HpTF 不同,请使用双耳状态查询");
        }
        write_sized_output(out, to_hptf_info(info.left));
    } else {
        write_sized_output(out,
                           adm_hptf_stereo_info_t{
                               sizeof(adm_hptf_stereo_info_t), 0, to_hptf_info(info.left), to_hptf_info(info.right)});
    }
    return {};
}
// All four wire forms are validated and copied into an owned Rust parameter snapshot.
template <typename Session>
mradm::Result<void> apply_hptf_request(Session& session, const void* config, uint32_t kind) {
    auto settings = mradm::render_common::prepare_hptf(config, kind);
    return settings ? session.set_hptf_prepared(*settings) : tl::unexpected{settings.error()};
}
adm_error_code_t monitor_hptf(adm_monitor_t* monitor, const void* config, uint32_t kind) noexcept {
    if (monitor == nullptr || !monitor->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(monitor);
        auto result = apply_hptf_request(*monitor->session, config, kind);
        if (!result) {
            store_last_error(monitor, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
template <typename T> adm_error_code_t monitor_hptf_info(adm_monitor_t* monitor, T* out) noexcept {
    if (monitor == nullptr || !monitor->session || !output_struct_valid(out)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto result = write_hptf_info(out, monitor->session->hptf_stereo_info());
        if (!result) {
            store_last_error(monitor, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
} // namespace

adm_error_code_t adm_hptf_parse_parametric_eq(adm_context_t* context,
                                              const char* text,
                                              double* out_preamp_db,
                                              adm_hptf_band_t* out_bands,
                                              uint32_t capacity,
                                              uint32_t* out_count) noexcept {
    if (context == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(context);
        const auto invalid = [context](const char* message) {
            store_last_error(context, {mradm::ErrorCode::invalid_argument, message, {}});
            return ADM_ERROR_INVALID_ARGUMENT;
        };
        if (text == nullptr || out_preamp_db == nullptr || out_count == nullptr ||
            (out_bands == nullptr && capacity != 0)) {
            return invalid("HpTF:解析输入或输出指针无效");
        }
        auto profile = mradm::parse_hptf_parametric_eq(text);
        if (!profile) {
            store_last_error(context, profile.error());
            return map_error(profile.error().code);
        }
        if (profile->bands.size() > std::numeric_limits<uint32_t>::max()) {
            return invalid("HpTF:解析结果的段数超出范围");
        }
        const auto count = static_cast<uint32_t>(profile->bands.size());
        if (out_bands == nullptr && capacity == 0) {
            *out_count = count;
            *out_preamp_db = profile->preamp_db;
            return ADM_ERROR_OK;
        }
        if (capacity < count) {
            const auto code = invalid("HpTF:输出数组容量不足");
            *out_count = count;
            *out_preamp_db = profile->preamp_db;
            return code;
        }
        uint32_t stride = 0;
        if (!hptf_output_stride(out_bands, count, stride)) {
            return invalid("HpTF:输出数组的 struct_size 或 stride 无效");
        }
        auto* bytes = reinterpret_cast<std::byte*>(out_bands);
        for (uint32_t index = 0; index < count; ++index) {
            const auto& band = profile->bands[index];
            const adm_hptf_band_t value{
                stride, static_cast<int32_t>(band.type), band.enabled ? 1 : 0, 0, band.fc_hz, band.gain_db, band.q};
            std::memcpy(bytes + (static_cast<std::size_t>(index) * stride),
                        &value,
                        std::min<std::size_t>(stride, sizeof(value)));
        }
        *out_count = count;
        *out_preamp_db = profile->preamp_db;
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_monitor_set_hptf_parameters(adm_monitor_t* monitor,
                                                 const adm_hptf_parameters_t* parameters) noexcept {
    return monitor_hptf(monitor, parameters, 1);
}
adm_error_code_t adm_monitor_set_hptf(adm_monitor_t* monitor, const adm_hptf_config_t* config) noexcept {
    return monitor_hptf(monitor, config, 0);
}
adm_error_code_t adm_monitor_set_hptf_stereo(adm_monitor_t* monitor, const adm_hptf_stereo_config_t* config) noexcept {
    return monitor_hptf(monitor, config, 2);
}
adm_error_code_t adm_monitor_set_hptf_stereo_parameters(adm_monitor_t* monitor,
                                                        const adm_hptf_stereo_parameters_t* parameters) noexcept {
    return monitor_hptf(monitor, parameters, 3);
}
adm_error_code_t adm_monitor_get_hptf_info(adm_monitor_t* monitor, adm_hptf_info_t* out) noexcept {
    return monitor_hptf_info(monitor, out);
}
adm_error_code_t adm_monitor_get_hptf_stereo_info(adm_monitor_t* monitor, adm_hptf_stereo_info_t* out) noexcept {
    return monitor_hptf_info(monitor, out);
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

/* ── Probe ───────────────────────────────────────────────────────────────── */

/* ── Scene inspect (JSON) ────────────────────────────────────────────────── */

/* ── Capabilities (JSON) ─────────────────────────────────────────────────── */

// cppcheck-suppress-end constParameterPointer

/* ── Producer-neutral realtime Scene stream (v1.35) ─────────────────────── */

adm_error_code_t adm_create_scene_stream(adm_context_t* context,
                                         const adm_scene_stream_config_t* config,
                                         adm_scene_stream_t** out) noexcept {
    return adm_create_scene_stream_with_room(context, config, nullptr, out);
}

adm_error_code_t adm_create_scene_stream_with_room(adm_context_t* context,
                                                   const adm_scene_stream_config_t* config,
                                                   const adm_room_config_t* room,
                                                   adm_scene_stream_t** out) noexcept {
    return adm_create_scene_stream_with_room_ex(context, config, room, nullptr, out);
}

adm_error_code_t adm_create_scene_stream_with_room_ex(adm_context_t* context,
                                                      const adm_scene_stream_config_t* config,
                                                      const adm_room_config_t* room,
                                                      const char* screen_json,
                                                      adm_scene_stream_t** out) noexcept {
    return adm_create_scene_stream_with_room_quality(context, config, room, ADM_ROOM_QUALITY_EXACT, screen_json, out);
}

adm_error_code_t adm_create_scene_stream_with_room_quality(adm_context_t* context,
                                                           const adm_scene_stream_config_t* config,
                                                           const adm_room_config_t* room,
                                                           adm_room_quality_t quality,
                                                           const char* screen_json,
                                                           adm_scene_stream_t** out) noexcept {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (context == nullptr || config == nullptr || out == nullptr || config->struct_size < sizeof(*config) ||
        !valid_room_quality(quality)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(context);
        adm_scene_stream_config_t input{};
        std::memcpy(&input, config, sizeof(input));
        auto renderer = to_scene_renderer_config(&input.rendering, input.input_sample_rate);
        if (!renderer) {
            store_last_error(context, renderer.error());
            return map_error(renderer.error().code);
        }

        mradm::realtime::SceneStreamConfig converted;
        converted.renderer = std::move(*renderer);
        if (quality != ADM_ROOM_QUALITY_EXACT &&
            converted.renderer.renderer != mradm::RendererSelection::room_binaural) {
            return ADM_ERROR_UNSUPPORTED;
        }
        if (screen_json != nullptr) {
            if (converted.renderer.renderer != mradm::RendererSelection::room_binaural) {
                return ADM_ERROR_UNSUPPORTED;
            }
            auto screens = mradm::parse_screen_config_json(screen_json);
            if (!screens) {
                store_last_error(context, screens.error());
                return map_error(screens.error().code);
            }
            converted.renderer.screens = *screens;
        }
        if (room != nullptr || converted.renderer.renderer == mradm::RendererSelection::room_binaural) {
            auto room_config = to_room_config(room);
            if (!room_config) {
                store_last_error(context, room_config.error());
                return map_error(room_config.error().code);
            }
            if (converted.renderer.renderer != mradm::RendererSelection::room_binaural) {
                return ADM_ERROR_INVALID_ARGUMENT;
            }
            room_config->quality = static_cast<mradm::RoomQuality>(quality);
            converted.renderer.room = std::move(*room_config);
        }
        converted.output_sample_rate = input.output_sample_rate;
        converted.input_queue_samples = input.input_queue_samples;
        converted.input_queue_bytes = input.input_queue_bytes;
        converted.output_ring_frames = input.output_ring_frames;
        converted.startup_watermark_frames = input.startup_watermark_frames;

        auto created = mradm::realtime::SceneStreamEngine::create(converted);
        if (!created) {
            store_last_error(context, created.error());
            return map_error(created.error().code);
        }
        auto handle = std::make_unique<adm_scene_stream_t>();
        handle->engine = std::move(*created);
        *out = handle.release();
        return ADM_ERROR_OK;
    } catch (const std::exception& exception) {
        set_last_error(context,
                       std::string{"unexpected exception while creating live Scene stream: "} + exception.what());
        return ADM_ERROR_INTERNAL;
    } catch (...) {
        set_last_error(context, "unexpected exception while creating live Scene stream");
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
    if (stream == nullptr || !stream->engine || stream->engine->output_attached()) {
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
            if (descriptor.semantic_identity != nullptr) {
                mradm::live_scene::SemanticIdentity identity;
                if (!to_scene_semantic_identity(descriptor.semantic_identity, identity)) {
                    store_last_error_message(stream, "invalid live Scene semantic identity or membership array");
                    return ADM_ERROR_INVALID_ARGUMENT;
                }
                element.semantic_identity = std::move(identity);
            }
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
                                    update.jump_position != 0,
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
    if (stream == nullptr || !stream->engine || stream->engine->output_attached() || !output_struct_valid(result) ||
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
    converted.semantic_policy_revision = status.semantic_policy_revision;
    converted.ring_fill = status.ring_fill;
    converted.ended = status.ended ? 1 : 0;
    converted.failed = status.failed ? 1 : 0;
    write_sized_output(out, converted);
    return ADM_ERROR_OK;
}

adm_error_code_t adm_scene_stream_switch_backend(adm_scene_stream_t* stream,
                                                 const adm_scene_renderer_config_t* config) noexcept {
    if (stream == nullptr || !stream->engine) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        auto converted = to_scene_renderer_config(config, stream->engine->input_sample_rate());
        if (!converted) {
            store_last_error(stream, converted.error());
            return map_error(converted.error().code);
        }
        auto result = stream->engine->switch_backend(*converted);
        if (!result) {
            store_last_error(stream, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while switching live Scene backend");
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_scene_stream_set_listener_orientation(adm_scene_stream_t* stream,
                                                           float yaw_deg,
                                                           float pitch_deg,
                                                           float roll_deg) noexcept {
    if (stream == nullptr || !stream->engine || !std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) ||
        !std::isfinite(roll_deg)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        mradm::ListenerOrientation orientation;
        orientation.yaw_deg = yaw_deg;
        orientation.pitch_deg = pitch_deg;
        orientation.roll_deg = roll_deg;
        stream->engine->set_listener_orientation(orientation);
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while setting live Scene listener orientation");
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_scene_stream_switch_backend_ex(const adm_scene_stream_t* stream,
                                                    const adm_scene_renderer_config_t* config,
                                                    char** out_error) noexcept {
    if (out_error == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    *out_error = nullptr;
    if (stream == nullptr || !stream->engine) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto fail = [out_error](const mradm::Error& error) {
            owned_string(error.message, out_error);
            return map_error(error.code);
        };
        auto converted = to_scene_renderer_config(config, stream->engine->input_sample_rate());
        if (!converted) {
            return fail(converted.error());
        }
        auto result = stream->engine->switch_backend(*converted);
        return result ? ADM_ERROR_OK : fail(result.error());
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t
adm_scene_stream_set_semantic_policy_json(adm_scene_stream_t* stream, const char* json, uint64_t revision) noexcept {
    if (stream == nullptr || !stream->engine) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        clear_last_error(stream);
        const std::string_view document = json != nullptr ? std::string_view{json} : std::string_view{};
        auto result = stream->engine->set_semantic_policy_json(document, revision);
        if (!result) {
            store_last_error(stream, result.error());
            return map_error(result.error().code);
        }
        return ADM_ERROR_OK;
    } catch (...) {
        store_last_error_message(stream, "unexpected exception while setting live Scene semantic policy");
        return ADM_ERROR_INTERNAL;
    }
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

namespace {
template <typename Fn> adm_error_code_t scene_output_call(adm_scene_output_t* output, Fn&& action) noexcept {
    if (output == nullptr || !output->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        const std::lock_guard lock(output->mutex);
        auto result = std::forward<Fn>(action)(*output->session);
        output->message = result ? std::string{} : result.error().message;
        return result ? ADM_ERROR_OK : map_error(result.error().code);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
} // namespace

adm_error_code_t adm_create_scene_output(adm_context_t* context,
                                         const adm_scene_stream_t* stream,
                                         const adm_scene_output_config_t* config,
                                         adm_scene_output_t** out) noexcept {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (context == nullptr || stream == nullptr || !stream->engine || config == nullptr || out == nullptr ||
        config->struct_size < sizeof(*config) || config->kind < ADM_SCENE_OUTPUT_SYSTEM_SPATIAL ||
        config->kind > ADM_SCENE_OUTPUT_NULL || config->speaker_geometry < ADM_SPEAKER_GEOMETRY_STANDARD ||
        config->speaker_geometry > ADM_SPEAKER_GEOMETRY_APPLE) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        mradm::realtime::SceneDeviceConfig options;
        options.kind = static_cast<mradm::realtime::SceneOutputKind>(config->kind);
        options.layout = config->output_layout == nullptr ? "" : config->output_layout;
        options.device_id = config->device_id == nullptr ? "" : config->device_id;
        options.geometry = static_cast<mradm::SpeakerGeometry>(config->speaker_geometry);
        auto session = mradm::realtime::SceneOutputSession::create(stream->engine, options);
        if (!session) {
            store_last_error(context, session.error());
            return map_error(session.error().code);
        }
        auto output = std::make_unique<adm_scene_output_t>();
        output->session = std::move(*session);
        *out = output.release();
        return ADM_ERROR_OK;
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

void adm_destroy_scene_output(adm_scene_output_t* output) noexcept {
    delete output;
}

const char* adm_scene_output_last_error_message(const adm_scene_output_t* output) noexcept {
    if (output == nullptr) {
        return "";
    }
    const std::lock_guard lock(output->mutex);
    return output->message.c_str();
}

adm_error_code_t adm_scene_output_play(adm_scene_output_t* output) noexcept {
    return scene_output_call(output, [](auto& session) -> mradm::Result<void> {
        session.play();
        return {};
    });
}

adm_error_code_t adm_scene_output_pause(adm_scene_output_t* output) noexcept {
    return scene_output_call(output, [](auto& session) -> mradm::Result<void> {
        session.pause();
        return {};
    });
}

adm_error_code_t
adm_scene_output_begin_epoch(adm_scene_output_t* output, uint64_t epoch, int64_t target_sample) noexcept {
    return scene_output_call(output, [=](auto& session) { return session.begin_epoch(epoch, target_sample); });
}

adm_error_code_t adm_scene_output_set_volume(adm_scene_output_t* output, float gain) noexcept {
    return scene_output_call(output, [=](auto& session) { return session.set_volume(gain); });
}

namespace {
adm_error_code_t scene_hptf(adm_scene_output_t* output, const void* config, uint32_t kind) noexcept {
    return scene_output_call(output, [=](auto& session) { return apply_hptf_request(session, config, kind); });
}
adm_error_code_t
scene_hptf_ex(const adm_scene_output_t* output, const void* config, uint32_t kind, char** out_error) noexcept {
    if (out_error == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    *out_error = nullptr;
    if (output == nullptr || !output->session) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto result = apply_hptf_request(*output->session, config, kind);
        if (result) {
            return ADM_ERROR_OK;
        }
        owned_string(result.error().message, out_error);
        return map_error(result.error().code);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
template <typename T> adm_error_code_t scene_hptf_info(adm_scene_output_t* output, T* out) noexcept {
    if (!output_struct_valid(out)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    return scene_output_call(output, [out](auto& session) { return write_hptf_info(out, session.hptf_stereo_info()); });
}
} // namespace
adm_error_code_t adm_scene_output_set_hptf(adm_scene_output_t* output, const adm_hptf_config_t* config) noexcept {
    return scene_hptf(output, config, 0);
}
adm_error_code_t
adm_scene_output_set_hptf_ex(const adm_scene_output_t* output, const adm_hptf_config_t* config, char** error) noexcept {
    return scene_hptf_ex(output, config, 0, error);
}
adm_error_code_t adm_scene_output_set_hptf_parameters(adm_scene_output_t* output,
                                                      const adm_hptf_parameters_t* parameters) noexcept {
    return scene_hptf(output, parameters, 1);
}
adm_error_code_t adm_scene_output_set_hptf_stereo(adm_scene_output_t* output,
                                                  const adm_hptf_stereo_config_t* config) noexcept {
    return scene_hptf(output, config, 2);
}
adm_error_code_t adm_scene_output_set_hptf_stereo_ex(const adm_scene_output_t* output,
                                                     const adm_hptf_stereo_config_t* config,
                                                     char** error) noexcept {
    return scene_hptf_ex(output, config, 2, error);
}
adm_error_code_t adm_scene_output_set_hptf_stereo_parameters(adm_scene_output_t* output,
                                                             const adm_hptf_stereo_parameters_t* parameters) noexcept {
    return scene_hptf(output, parameters, 3);
}
adm_error_code_t adm_scene_output_get_hptf_info(adm_scene_output_t* output, adm_hptf_info_t* out) noexcept {
    return scene_hptf_info(output, out);
}
adm_error_code_t adm_scene_output_get_hptf_stereo_info(adm_scene_output_t* output,
                                                       adm_hptf_stereo_info_t* out) noexcept {
    return scene_hptf_info(output, out);
}

namespace {
adm_error_code_t room_switch(const std::shared_ptr<mradm::IRoomControl>& control,
                             const adm_room_config_t* config,
                             uint64_t* revision,
                             char** error) {
    if (revision == nullptr || error == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    *revision = 0;
    *error = nullptr;
    if (!control) {
        return ADM_ERROR_UNSUPPORTED;
    }
    auto converted = to_room_config(config);
    if (!converted) {
        owned_string(converted.error().message, error);
        return map_error(converted.error().code);
    }
    converted->quality = control->status().quality;
    auto result = control->request(*converted);
    if (!result) {
        owned_string(result.error().message, error);
        return map_error(result.error().code);
    }
    *revision = *result;
    return ADM_ERROR_OK;
}
adm_error_code_t room_status_json(const std::shared_ptr<mradm::IRoomControl>& control, char** out) {
    if (out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (!control) {
        return ADM_ERROR_UNSUPPORTED;
    }
    const auto status = control->status();
    constexpr std::array names{"applied", "preparing", "transitioning", "draining", "failed"};
    nlohmann::json json{{"schema", "mradm.room-status"},
                        {"version", 1},
                        {"quality", mradm::room_quality_name(status.quality)},
                        {"reflection_update_frames", status.reflection_update_frames},
                        {"state", names.at(static_cast<std::size_t>(status.state))},
                        {"requested_revision", status.requested_revision},
                        {"applied_revision", status.applied_revision},
                        {"transition_sample", status.transition_sample},
                        {"material_frames", status.material_frames},
                        {"tail_frames", status.tail_frames},
                        {"buffer_frames", status.buffer_frames},
                        {"material_filter_guard_frames", status.material_filter_guard_frames},
                        {"constrained_sources", status.constrained_sources},
                        {"message", status.message}};
    owned_string(json.dump(), out);
    return ADM_ERROR_OK;
}
adm_error_code_t room_source_json(const std::shared_ptr<mradm::IRoomControl>& control, uint64_t id, char** out) {
    if (out == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (!control) {
        return ADM_ERROR_UNSUPPORTED;
    }
    const auto source = control->source_position(id);
    nlohmann::json json{{"schema", "mradm.room-source"},
                        {"version", 1},
                        {"element_id", id},
                        {"position_metres", source.position_metres},
                        {"direction", source.direction},
                        {"distance_metres", source.distance_metres},
                        {"constrained", source.constrained},
                        {"present", source.present}};
    owned_string(json.dump(), out);
    return ADM_ERROR_OK;
}
} // namespace

adm_error_code_t adm_monitor_switch_room(adm_monitor_t* monitor,
                                         const adm_room_config_t* config,
                                         uint64_t* out_request,
                                         char** out_error) noexcept {
    if (out_request != nullptr) {
        *out_request = 0;
    }
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
    if (monitor == nullptr || monitor->session == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        return room_switch(monitor->session->room_control(), config, out_request, out_error);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
adm_error_code_t adm_scene_stream_switch_room(const adm_scene_stream_t* stream,
                                              const adm_room_config_t* config,
                                              uint64_t* out_request,
                                              char** out_error) noexcept {
    if (out_request != nullptr) {
        *out_request = 0;
    }
    if (out_error != nullptr) {
        *out_error = nullptr;
    }
    if (stream == nullptr || stream->engine == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        return room_switch(stream->engine->room_control(), config, out_request, out_error);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
adm_error_code_t adm_monitor_room_status_json(const adm_monitor_t* monitor, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (monitor == nullptr || monitor->session == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        return room_status_json(monitor->session->room_control(), out_json);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
adm_error_code_t adm_scene_stream_room_status_json(const adm_scene_stream_t* stream, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (stream == nullptr || stream->engine == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        return room_status_json(stream->engine->room_control(), out_json);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
adm_error_code_t
adm_monitor_room_source_json(const adm_monitor_t* monitor, uint64_t element_id, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (monitor == nullptr || monitor->session == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        return room_source_json(monitor->session->room_control(), element_id, out_json);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}
adm_error_code_t
adm_scene_stream_room_source_json(const adm_scene_stream_t* stream, uint64_t element_id, char** out_json) noexcept {
    if (out_json != nullptr) {
        *out_json = nullptr;
    }
    if (stream == nullptr || stream->engine == nullptr) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    try {
        return room_source_json(stream->engine->room_control(), element_id, out_json);
    } catch (...) {
        return ADM_ERROR_INTERNAL;
    }
}

adm_error_code_t adm_scene_output_get_status(adm_scene_output_t* output, adm_scene_output_status_t* out) noexcept {
    if (!output_struct_valid(out)) {
        return ADM_ERROR_INVALID_ARGUMENT;
    }
    return scene_output_call(output, [out](auto& session) -> mradm::Result<void> {
        const auto status = session.status();
        adm_scene_output_status_t value{};
        value.state = static_cast<int32_t>(status.state);
        value.epoch_id = status.epoch_id;
        value.consumed_frames = status.consumed_frames;
        value.presented_frames = status.presented_frames;
        value.queued_frames = status.queued_frames;
        value.underruns = status.underruns;
        value.clock_kind = status.media_clock ? ADM_SCENE_OUTPUT_CLOCK_MEDIA : ADM_SCENE_OUTPUT_CLOCK_CALLBACK;
        value.recovering = status.recovering ? 1 : 0;
        value.failed = status.failed ? 1 : 0;
        value.ended = status.ended ? 1 : 0;
        write_sized_output(out, value);
        return {};
    });
}
