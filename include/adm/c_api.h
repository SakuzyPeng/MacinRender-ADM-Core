#pragma once

#include <stdint.h>

/*
 * MacinRender ADM C ABI — stable v1.
 *
 * STATUS: stable — 自 v1.0.0 起承诺向后二进制兼容。
 * 兼容规则：允许追加新函数、enum 末尾新值、opaque struct 内部变化；
 *            禁止修改现有 signature、enum 已有值、callback 形参。
 *            破坏性变更须发布 v2.0.0 并更新 SONAME。
 * 完整策略：docs/adr/0007-c-abi-stability-policy.md
 *
 * v1.1 新增（additive，SOVERSION 不变）：
 *   adm_renderer_t, adm_output_bit_depth_t, adm_speaker_spread_mode_t,
 *   adm_binaural_spread_mode_t, adm_iamf_container_t,
 *   adm_render_options_t + builder/setter/destroy,
 *   adm_render_file_ex, adm_render_result_output_path/loudness_lufs/peak_dbtp,
 *   adm_scene_info_t + adm_probe_file + accessors,
 *   adm_log_level_t + adm_render_result_log_count/log_entry,
 *   adm_inspect_file_json + adm_free_string, adm_capabilities_json,
 *   adm_layouts_json, adm_inspect_file_xml, adm_policy_template_json.
 *
 * v1.2 新增（additive，SOVERSION 不变）：
 *   adm_render_options_set_render_start_sec / adm_render_options_set_render_end_sec.
 *
 * v1.3 新增（additive，SOVERSION 不变）：
 *   adm_render_options_set_final_gain_db.
 *
 * v1.4 新增（additive，SOVERSION 不变）：
 *   adm_cancel_token_t + adm_create_cancel_token / adm_cancel /
 *   adm_reset_cancel_token / adm_destroy_cancel_token,
 *   adm_render_options_set_cancel_token.
 *
 * v1.5 新增（additive，SOVERSION 不变）：
 *   adm_render_options_set_semantic_policy_json,
 *   adm_render_options_set_capture_semantic_report,
 *   adm_render_result_semantic_report_json.
 *
 * v1.6 新增（additive，SOVERSION 不变）：
 *   adm_output_formats_json.
 *
 * v1.7 新增（additive，SOVERSION 不变）：
 *   adm_render_stage_t + adm_render_stage_from_string.
 *
 * v1.8 新增（additive，SOVERSION 不变）：
 *   adm_preview_session_t + adm_create_preview_session /
 *   adm_preview_render_window / adm_destroy_preview_session.
 *
 * v1.9 新增（additive，SOVERSION 不变）：
 *   adm_apac_container_t + adm_render_options_set_apac_container.
 *
 * v1.10 新增（additive，SOVERSION 不变）：
 *   adm_progress_operation_t + adm_progress_event_v2_t + adm_progress_v2_cb,
 *   adm_render_file_ex2, adm_preview_render_window_v2.
 *
 * v1.11 新增（additive，SOVERSION 不变）：
 *   ADM_RENDERER_SAF_BINAURAL。
 *
 * v1.12 新增（additive，SOVERSION 不变）：
 *   adm_render_support_matrix_json.
 *
 * v1.13 新增（additive，SOVERSION 不变）：
 *   adm_export_file.
 *
 * v1.14 新增（additive，SOVERSION 不变）：
 *   adm_render_options_set_iamf_layers.
 *
 * v1.15 新增（additive，SOVERSION 不变）：
 *   adm_monitor_t + adm_create_monitor / adm_destroy_monitor / adm_monitor_play /
 *   adm_monitor_pause / adm_monitor_seek / adm_monitor_set_loop / adm_monitor_get_status /
 *   adm_monitor_get_levels / adm_monitor_log_count / adm_monitor_log_entry
 *   (实时监听引擎；状态/电平/日志均轮询，无回调)。
 *
 * v1.16 新增（additive，SOVERSION 不变）：
 *   adm_monitor_override_t + adm_monitor_set_overrides（实时按对象覆盖，gain 即时生效；
 *   diffuse/extent/divergence 缩放在 binaural 后端经廉价 re-prepare 生效，未接入的后端
 *   接受但忽略）。adm_monitor_status_t 追加 override_revision 字段（struct_size 向后兼容）。
 *
 * v1.17 新增（additive，SOVERSION 不变）：
 *   adm_monitor_switch_backend（实时热切换渲染后端 / 布局，带短交叉淡化。立体声监听下
 *   可把多声道 / HOA 折叠为立体声；非立体声监听且声道数不同、或采样率不同则返
 *   ADM_ERROR_UNSUPPORTED）。
 *
 * v1.18 新增（additive，SOVERSION 不变）：
 *   adm_monitor_levels_t 追加 momentary_lufs / shortterm_lufs / integrated_lufs 字段
 *   （监听输出的实时 LUFS，ITU-R BS.1770，经 libebur128；静音 / 低于门限时为 -inf。
 *   integrated 在每次 seek 后重新累计）。struct_size 守护：旧调用方按其 sizeof 仅读取
 *   已有字段，新字段仅在 struct_size 覆盖其偏移时写入。
 *
 * v1.19 新增（additive，SOVERSION 不变）：
 *   adm_monitor_override_t 追加 extent_width_scale / extent_height_scale / extent_depth_scale
 *   字段（在公共 extent_scale 之上对 width / height / depth 分别再缩放，实现 extent 三轴
 *   独立调节）。struct_size 守护：旧调用方按其 sizeof 仅传至 divergence_scale，缺省的三轴
 *   乘子按 1.0 处理（等价旧行为）；新字段仅在 struct_size 覆盖其偏移时读取。
 *
 * v1.20 新增（additive，SOVERSION 不变）：
 *   adm_monitor_override_t 追加 speaker_label 字段（const char*，可选 DirectSpeakers 声道过滤）。
 *   非空时该 override 只作用于 speaker label 匹配的那个声床声道，使单个声床（一个 audioObject、
 *   多声道）可按声道独立调 gain；NULL / "" 表示整对象（旧行为）。与导出端语义策略的
 *   DirectSpeakers speaker_label 过滤一致。struct_size 守护：旧调用方不带此字段按整对象处理。
 */

/* ── Version macros ──────────────────────────────────────────────────────── */

#define ADM_API_VERSION_MAJOR 1
#define ADM_API_VERSION_MINOR 20
#define ADM_API_VERSION_PATCH 0
#define ADM_API_VERSION ((ADM_API_VERSION_MAJOR * 10000) + (ADM_API_VERSION_MINOR * 100) + ADM_API_VERSION_PATCH)

/* ── Deprecation helper ───────────────────────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
#define ADM_API_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#define ADM_API_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#define ADM_API_DEPRECATED(msg)
#endif

/* ── Exception boundary ───────────────────────────────────────────────────── */
#ifdef __cplusplus
#define ADM_API_NOEXCEPT noexcept
#else
#define ADM_API_NOEXCEPT
#endif

/* ── C linkage ───────────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types ────────────────────────────────────────────────────────── */

typedef struct adm_context_t adm_context_t;
typedef struct adm_render_result_t adm_render_result_t;
typedef struct adm_render_options_t adm_render_options_t;   /* v1.1 */
typedef struct adm_scene_info_t adm_scene_info_t;           /* v1.1 */
typedef struct adm_cancel_token_t adm_cancel_token_t;       /* v1.4 */
typedef struct adm_preview_session_t adm_preview_session_t; /* v1.8 */

/* ── Error codes ─────────────────────────────────────────────────────────── */
/*
 * Stable: existing values must not be renumbered or removed.
 * New values may be appended before the closing brace in minor versions.
 */
typedef enum adm_error_code_t {
    ADM_ERROR_OK = 0,
    ADM_ERROR_INVALID_ARGUMENT = 1,
    ADM_ERROR_UNSUPPORTED = 2,
    ADM_ERROR_IO = 3,
    ADM_ERROR_RENDER_FAILED = 4,
    ADM_ERROR_CANCELLED = 5,
    ADM_ERROR_INTERNAL = 6
} adm_error_code_t;

#ifdef __cplusplus
static_assert(sizeof(adm_error_code_t) == sizeof(int), "adm_error_code_t must be int-sized for stable ABI");
#endif

/* ── v1.1 enums ──────────────────────────────────────────────────────────── */
/*
 * All v1.1 enums follow the same stability contract as adm_error_code_t:
 * existing values are frozen; new values may be appended in minor versions.
 * Booleans are expressed as int (0/1) throughout this header.
 */

typedef enum adm_renderer_t {
    ADM_RENDERER_AUTOMATIC = 0,
    ADM_RENDERER_EAR = 1,
    ADM_RENDERER_SAF = 2,
    ADM_RENDERER_HOA = 3,
    ADM_RENDERER_APPLE = 4,
    ADM_RENDERER_BINAURAL = 5,
    ADM_RENDERER_SAF_BINAURAL = 6
} adm_renderer_t;

typedef enum adm_output_bit_depth_t {
    ADM_BIT_DEPTH_F32 = 0,
    ADM_BIT_DEPTH_I24 = 1,
    ADM_BIT_DEPTH_I16 = 2
} adm_output_bit_depth_t;

typedef enum adm_speaker_spread_mode_t {
    ADM_SPEAKER_SPREAD_AUTOMATIC = 0,
    ADM_SPEAKER_SPREAD_NONE = 1,
    ADM_SPEAKER_SPREAD_MDAP = 2
} adm_speaker_spread_mode_t;

typedef enum adm_binaural_spread_mode_t {
    ADM_BINAURAL_SPREAD_AUTOMATIC = 0,
    ADM_BINAURAL_SPREAD_NONE = 1,
    ADM_BINAURAL_SPREAD_CLOUD = 2,
    ADM_BINAURAL_SPREAD_SAF_SPREADER = 3
} adm_binaural_spread_mode_t;

typedef enum adm_iamf_container_t { ADM_IAMF_CONTAINER_OBU = 0, ADM_IAMF_CONTAINER_MP4 = 1 } adm_iamf_container_t;

typedef enum adm_apac_container_t { ADM_APAC_CONTAINER_MPEG4 = 0, ADM_APAC_CONTAINER_CAF = 1 } adm_apac_container_t;

/* Severity of a captured diagnostic log entry (see adm_render_result_log_entry). */
typedef enum adm_log_level_t {
    ADM_LOG_DEBUG = 0,
    ADM_LOG_INFO = 1,
    ADM_LOG_WARNING = 2,
    ADM_LOG_ERROR = 3
} adm_log_level_t;

/* Render pipeline stage, the enum form of the `stage` string passed to
 * adm_progress_cb. Obtain it with adm_render_stage_from_string(stage) inside the
 * callback for phase-based progress UI / localization without string matching.
 * ADM_STAGE_UNKNOWN is returned for a NULL or unrecognized string. v1.7 */
typedef enum adm_render_stage_t {
    ADM_STAGE_UNKNOWN = 0,
    ADM_STAGE_VALIDATING = 1,
    ADM_STAGE_PROBING = 2,
    ADM_STAGE_IMPORTING_SCENE = 3,
    ADM_STAGE_PLANNING = 4,
    ADM_STAGE_RENDERING = 5,
    ADM_STAGE_POST_PROCESSING = 6,
    ADM_STAGE_FINISHED = 7
} adm_render_stage_t;

/* Fine-grained progress operation within a render stage. v1.10 */
typedef enum adm_progress_operation_t {
    ADM_PROGRESS_OPERATION_UNKNOWN = 0,
    ADM_PROGRESS_OPERATION_VALIDATE_REQUEST = 1,
    ADM_PROGRESS_OPERATION_PROBE_INPUT = 2,
    ADM_PROGRESS_OPERATION_IMPORT_SCENE = 3,
    ADM_PROGRESS_OPERATION_APPLY_SEMANTIC_POLICY = 4,
    ADM_PROGRESS_OPERATION_PLAN_RENDER = 5,
    ADM_PROGRESS_OPERATION_PREPARE_BACKEND = 6,
    ADM_PROGRESS_OPERATION_RENDER_AUDIO = 7,
    ADM_PROGRESS_OPERATION_TRIM_OUTPUT = 8,
    ADM_PROGRESS_OPERATION_APPLY_GAIN = 9,
    ADM_PROGRESS_OPERATION_CONVERT_BIT_DEPTH = 10,
    ADM_PROGRESS_OPERATION_ENCODE_FLAC = 11,
    ADM_PROGRESS_OPERATION_ENCODE_OPUS = 12,
    ADM_PROGRESS_OPERATION_ENCODE_APAC = 13,
    ADM_PROGRESS_OPERATION_ENCODE_IAMF = 14,
    ADM_PROGRESS_OPERATION_PACKAGE_IAMF_MP4 = 15,
    ADM_PROGRESS_OPERATION_WRITE_METADATA = 16,
    ADM_PROGRESS_OPERATION_FINISH = 17
} adm_progress_operation_t;

#ifdef __cplusplus
static_assert(sizeof(adm_renderer_t) == sizeof(int));
static_assert(sizeof(adm_output_bit_depth_t) == sizeof(int));
static_assert(sizeof(adm_speaker_spread_mode_t) == sizeof(int));
static_assert(sizeof(adm_binaural_spread_mode_t) == sizeof(int));
static_assert(sizeof(adm_iamf_container_t) == sizeof(int));
static_assert(sizeof(adm_apac_container_t) == sizeof(int));
static_assert(sizeof(adm_log_level_t) == sizeof(int));
static_assert(sizeof(adm_render_stage_t) == sizeof(int));
static_assert(sizeof(adm_progress_operation_t) == sizeof(int));
#endif

/* ── Progress callback ───────────────────────────────────────────────────── */
/*
 * Called synchronously from within adm_render_file / adm_render_file_ex;
 * never invoked after the function returns. stage and message are valid only
 * for the callback's duration. user_data is passed through unchanged.
 */
typedef void (*adm_progress_cb)(double fraction, const char* stage, const char* message, void* user_data);

/*
 * v1.10 structured progress event for GUI integrations.
 *
 * overall_fraction is the stable whole-render progress in [0, 1].
 * stage_fraction is progress within the current stage / operation in [0, 1].
 * current_frame / total_frames are set when the operation has frame-level
 * progress; otherwise both are 0. message is valid only for the callback's
 * duration, matching adm_progress_cb. struct_size is set by the library to
 * sizeof(adm_progress_event_v2_t), allowing additive tail fields in future
 * minor versions.
 */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct adm_progress_event_v2_t {
    uint32_t struct_size;
    adm_render_stage_t stage;
    adm_progress_operation_t operation;
    double overall_fraction;
    double stage_fraction;
    uint64_t current_frame;
    uint64_t total_frames;
    const char* message;
} adm_progress_event_v2_t;
/* cppcheck-suppress-end unusedStructMember */

typedef void (*adm_progress_v2_cb)(const adm_progress_event_v2_t* event, void* user_data);

/* Map a progress callback's `stage` string to adm_render_stage_t. Returns
 * ADM_STAGE_UNKNOWN for a NULL or unrecognized string. Pure, thread-safe, no
 * allocation — safe to call directly inside the progress callback. v1.7 */
adm_render_stage_t adm_render_stage_from_string(const char* stage) ADM_API_NOEXCEPT;

/* ── Runtime version query ───────────────────────────────────────────────── */
int adm_api_version_major(void) ADM_API_NOEXCEPT;
int adm_api_version_minor(void) ADM_API_NOEXCEPT;
int adm_api_version_patch(void) ADM_API_NOEXCEPT;

/* ── Context lifecycle ───────────────────────────────────────────────────── */
/*
 * adm_create_context / adm_destroy_context must be strictly paired.
 * A single context is not thread-safe; use one context per thread or
 * serialize access externally.
 */
adm_context_t* adm_create_context(void) ADM_API_NOEXCEPT;
void adm_destroy_context(adm_context_t* context) ADM_API_NOEXCEPT;

/* ── v1.1 Options builder ────────────────────────────────────────────────── */
/*
 * adm_create_render_options / adm_destroy_render_options must be strictly
 * paired. Passing NULL for opts to adm_render_file_ex is equivalent to
 * passing a freshly created options object with all defaults.
 *
 * Setter return values:
 *   ADM_ERROR_OK            — value accepted.
 *   ADM_ERROR_INVALID_ARGUMENT — unrecognised enum value, or NULL layout.
 *   ADM_ERROR_INTERNAL      — memory allocation failed (string/path setters).
 * When opts is NULL all setters silently no-op and return ADM_ERROR_OK.
 */
adm_render_options_t* adm_create_render_options(void) ADM_API_NOEXCEPT;
void adm_destroy_render_options(adm_render_options_t* opts) ADM_API_NOEXCEPT;

/* Returns ADM_ERROR_INVALID_ARGUMENT for unrecognised enum values. */
adm_error_code_t adm_render_options_set_renderer(adm_render_options_t* opts, adm_renderer_t renderer) ADM_API_NOEXCEPT;

/* output_layout: "5.1", "7.1.4", "hoa3", "binaural", etc. Copied internally.
 * Returns ADM_ERROR_INVALID_ARGUMENT if layout is NULL; may return ADM_ERROR_INTERNAL on OOM. */
adm_error_code_t adm_render_options_set_output_layout(adm_render_options_t* opts, const char* layout) ADM_API_NOEXCEPT;

/* Returns ADM_ERROR_INVALID_ARGUMENT for unrecognised enum values. */
adm_error_code_t adm_render_options_set_output_bit_depth(adm_render_options_t* opts,
                                                         adm_output_bit_depth_t depth) ADM_API_NOEXCEPT;

/* Setting a loudness target also enables loudness normalisation (measure_loudness=true).
 * Returns ADM_ERROR_INVALID_ARGUMENT if lufs is not in [-70, 0] or is non-finite. */
adm_error_code_t adm_render_options_set_loudness_target(adm_render_options_t* opts, double lufs) ADM_API_NOEXCEPT;

/* enabled: 1 = enable peak limiting (default), 0 = disable. */
void adm_render_options_set_peak_limit(adm_render_options_t* opts, int enabled) ADM_API_NOEXCEPT;
/* Returns ADM_ERROR_INVALID_ARGUMENT if dbtp is not in [-60, 0] or is non-finite. */
adm_error_code_t adm_render_options_set_peak_limit_dbtp(adm_render_options_t* opts, double dbtp) ADM_API_NOEXCEPT;

/* enabled: 1 = raise global gain up to peak_limit_dbtp when True Peak is below ceiling. */
void adm_render_options_set_peak_normalize_to_limit(adm_render_options_t* opts, int enabled) ADM_API_NOEXCEPT;

/* Opus MKA VBR target bitrate per channel in kbps. 0 = auto; otherwise [6, 320].
 * Returns ADM_ERROR_INVALID_ARGUMENT if kbps is outside [6, 320] and not 0. */
adm_error_code_t adm_render_options_set_opus_bitrate_per_ch_kbps(adm_render_options_t* opts,
                                                                 uint32_t kbps) ADM_API_NOEXCEPT;

/* APAC total target/hint bitrate in kbps (macOS only). 0 = layout default; otherwise [64, 32768].
 * Returns ADM_ERROR_INVALID_ARGUMENT if kbps is outside [64, 32768] and not 0. */
adm_error_code_t adm_render_options_set_apac_bitrate_kbps(adm_render_options_t* opts, uint32_t kbps) ADM_API_NOEXCEPT;

/* enabled: 1 = Music DRC profile (default), 0 = None. */
void adm_render_options_set_apac_drc_music(adm_render_options_t* opts, int enabled) ADM_API_NOEXCEPT;

/* APAC output container. MPEG4 writes .m4a/.mp4 (default); CAF writes APAC-in-CAF
 * and requires an output path with .caf extension at render time. v1.9 */
adm_error_code_t adm_render_options_set_apac_container(adm_render_options_t* opts,
                                                       adm_apac_container_t container) ADM_API_NOEXCEPT;

/* sofa_path: user SOFA HRIR file path for binaural rendering. NULL or "" = built-in KEMAR.
 * May return ADM_ERROR_INTERNAL on OOM. */
adm_error_code_t adm_render_options_set_sofa_path(adm_render_options_t* opts, const char* sofa_path) ADM_API_NOEXCEPT;

/* semantic_policy_path / semantic_report_path: NULL or "" clears the field.
 * May return ADM_ERROR_INTERNAL on OOM. */
adm_error_code_t adm_render_options_set_semantic_policy_path(adm_render_options_t* opts,
                                                             const char* path) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_options_set_semantic_report_path(adm_render_options_t* opts,
                                                             const char* path) ADM_API_NOEXCEPT;

/* semantic_policy_json: an in-memory semantic-policy document (UTF-8 JSON, schema
 * "mradm.semantic-policy.v1"). When set it takes precedence over any
 * semantic_policy_path (a warning is logged if both are present), letting a GUI
 * apply an edited policy without writing a temp file. The string is copied
 * internally. NULL or "" clears the field. May return ADM_ERROR_INTERNAL on OOM.
 * Malformed JSON is not diagnosed here — it surfaces as a render error.
 * v1.5 */
adm_error_code_t adm_render_options_set_semantic_policy_json(adm_render_options_t* opts,
                                                             const char* json) ADM_API_NOEXCEPT;

/* enabled: 1 = also capture the effective semantic report in-memory, retrievable
 * after the render via adm_render_result_semantic_report_json; 0 = do not (default).
 * Independent of semantic_report_path (which writes a file copy). When opts is NULL
 * this is a safe no-op.
 * v1.5 */
void adm_render_options_set_capture_semantic_report(adm_render_options_t* opts, int enabled) ADM_API_NOEXCEPT;

/* default_interp_ms: gain-interpolation ramp when ADM block carries no interpolationLength. [0, 500].
 * Returns ADM_ERROR_INVALID_ARGUMENT if ms > 500. */
adm_error_code_t adm_render_options_set_default_interp_ms(adm_render_options_t* opts, uint32_t ms) ADM_API_NOEXCEPT;

/* object_smoothing_frames: control-rate de-zipper window in sample frames. 0 = disabled. [0, 48000].
 * Returns ADM_ERROR_INVALID_ARGUMENT if frames > 48000. */
adm_error_code_t adm_render_options_set_object_smoothing_frames(adm_render_options_t* opts,
                                                                uint32_t frames) ADM_API_NOEXCEPT;

/* Returns ADM_ERROR_INVALID_ARGUMENT for unrecognised enum values. */
adm_error_code_t adm_render_options_set_speaker_spread_mode(adm_render_options_t* opts,
                                                            adm_speaker_spread_mode_t mode) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_options_set_binaural_spread_mode(adm_render_options_t* opts,
                                                             adm_binaural_spread_mode_t mode) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_options_set_iamf_container(adm_render_options_t* opts,
                                                       adm_iamf_container_t container) ADM_API_NOEXCEPT;
/* iamf_layers_csv: optional IAMF scalable channel layers, comma-separated
 * ("5.1,5.1.2,5.1.4,7.1.4"). NULL or "" clears the list and preserves the
 * default single-layer output_layout behavior. v1.14 */
adm_error_code_t adm_render_options_set_iamf_layers(adm_render_options_t* opts,
                                                    const char* iamf_layers_csv) ADM_API_NOEXCEPT;

/* render_start_sec / render_end_sec: output time-range trim in seconds on the
 * rendered timeline (which equals the input timeline). Loudness/True-Peak are
 * measured over the trimmed segment.
 *   start: must be finite and >= 0. Returns ADM_ERROR_INVALID_ARGUMENT otherwise.
 *   end:   sec <= 0 clears it (render to the end); a positive end must be greater
 *          than start, which is validated at render time. Non-finite returns
 *          ADM_ERROR_INVALID_ARGUMENT.
 * v1.2 */
adm_error_code_t adm_render_options_set_render_start_sec(adm_render_options_t* opts, double sec) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_options_set_render_end_sec(adm_render_options_t* opts, double sec) ADM_API_NOEXCEPT;

/* final_gain_db: unconstrained final gain in dB, applied after all automatic gain
 * staging (loudness / peak). It bypasses peak limiting and may push the signal
 * above the peak ceiling and above 0 dBFS (integer outputs can clip); it is
 * reflected in the reported metrics and file metadata. 0 = no-op. No range limit;
 * a non-finite value returns ADM_ERROR_INVALID_ARGUMENT.
 * v1.3 */
adm_error_code_t adm_render_options_set_final_gain_db(adm_render_options_t* opts, double db) ADM_API_NOEXCEPT;

/* ── v1.4 Cancellation ───────────────────────────────────────────────────── */
/*
 * Cooperative cancellation for a running render. Typical use: the GUI creates one
 * token, passes it to the render options, starts adm_render_file_ex on a worker
 * thread, and calls adm_cancel from the UI thread when the user clicks "Cancel".
 * The active render then aborts at the next internal chunk/stage boundary and
 * returns ADM_ERROR_CANCELLED; any partially written output file is removed.
 *
 * adm_create_cancel_token / adm_destroy_cancel_token must be strictly paired.
 * The token may be reused across renders: call adm_reset_cancel_token between
 * renders to clear a prior cancellation. A token must outlive every render whose
 * options reference it (it is borrowed, not owned, by the options object).
 *
 * THREAD SAFETY: unlike adm_context_t, a cancel token is explicitly safe to use
 * from a thread other than the one running the render — adm_cancel may be called
 * concurrently with an in-flight adm_render_file_ex that references the token.
 * adm_create / adm_reset / adm_destroy are NOT thread-safe relative to a render
 * in progress and must not race with it (reset/destroy only once the render has
 * returned). adm_cancel is idempotent.
 */
adm_cancel_token_t* adm_create_cancel_token(void) ADM_API_NOEXCEPT;
void adm_destroy_cancel_token(adm_cancel_token_t* token) ADM_API_NOEXCEPT;

/* Request cancellation. Idempotent; thread-safe relative to a running render.
 * Passing NULL is a safe no-op. */
void adm_cancel(adm_cancel_token_t* token) ADM_API_NOEXCEPT;

/* Clear a prior cancellation so the token can drive a fresh render. Must not race
 * with a render in progress. Passing NULL is a safe no-op. */
void adm_reset_cancel_token(adm_cancel_token_t* token) ADM_API_NOEXCEPT;

/* Associate a cancel token with these render options. The token is borrowed and
 * must outlive any render using these options. Passing token == NULL clears the
 * association (the render becomes non-cancellable). When opts is NULL this is a
 * safe no-op. */
void adm_render_options_set_cancel_token(adm_render_options_t* opts, adm_cancel_token_t* token) ADM_API_NOEXCEPT;

/* ── Render ──────────────────────────────────────────────────────────────── */
/*
 * adm_render_file — v1.0 entry point, preserved for binary compatibility.
 * Equivalent to adm_render_file_ex with opts == NULL (all defaults).
 * output_path may be NULL to derive a path automatically.
 * progress may be NULL. result may be NULL to suppress result allocation.
 */
adm_error_code_t adm_render_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 adm_progress_cb progress,
                                 void* user_data,
                                 adm_render_result_t** result) ADM_API_NOEXCEPT;

/*
 * adm_render_file_ex — v1.1 entry point with full options support.
 * opts may be NULL (equivalent to all defaults).
 * All other parameters behave identically to adm_render_file.
 */
adm_error_code_t adm_render_file_ex(adm_context_t* context,
                                    const char* input_path,
                                    const char* output_path,
                                    const adm_render_options_t* opts,
                                    adm_progress_cb progress,
                                    void* user_data,
                                    adm_render_result_t** result) ADM_API_NOEXCEPT;

/*
 * adm_render_file_ex2 — v1.10 entry point with structured progress events.
 * All parameters behave like adm_render_file_ex, except progress uses
 * adm_progress_v2_cb. opts may be NULL (equivalent to all defaults).
 */
adm_error_code_t adm_render_file_ex2(adm_context_t* context,
                                     const char* input_path,
                                     const char* output_path,
                                     const adm_render_options_t* opts,
                                     adm_progress_v2_cb progress,
                                     void* user_data,
                                     adm_render_result_t** result) ADM_API_NOEXCEPT;

/* ── Result lifecycle ────────────────────────────────────────────────────── */
/*
 * Call adm_destroy_render_result for every non-NULL result handle.
 * All const char* values returned by result accessors are owned by the
 * result handle and remain valid until adm_destroy_render_result is called.
 */
void adm_destroy_render_result(adm_render_result_t* result) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_result_error_code(const adm_render_result_t* result) ADM_API_NOEXCEPT;
const char* adm_render_result_message(const adm_render_result_t* result) ADM_API_NOEXCEPT;

/* v1.1 result accessors */

/* Returns the actual output file path written (including auto-derived paths). NULL if result is NULL. */
const char* adm_render_result_output_path(const adm_render_result_t* result) ADM_API_NOEXCEPT;

/*
 * adm_render_result_loudness_lufs / adm_render_result_peak_dbtp:
 * Returns 1 and writes the value to *out_value if the metric is present.
 * Returns 0 if the metric is absent (silence, signal too short for gating,
 * or loudness measurement was not requested). out_value may be NULL.
 */
int adm_render_result_loudness_lufs(const adm_render_result_t* result, double* out_value) ADM_API_NOEXCEPT;
int adm_render_result_peak_dbtp(const adm_render_result_t* result, double* out_value) ADM_API_NOEXCEPT;

/*
 * adm_render_result_semantic_report_json (v1.5): the effective semantic-policy
 * report (UTF-8 JSON, schema "mradm.semantic-report.v1") captured during the
 * render, if the request enabled it via adm_render_options_set_capture_semantic_report.
 * Returns NULL if not captured (flag off) or if result is NULL. The string is owned
 * by the result handle and remains valid until adm_destroy_render_result (do NOT
 * pass it to adm_free_string). Available regardless of the render's error code, so a
 * GUI can show the report even when a later stage failed.
 */
const char* adm_render_result_semantic_report_json(const adm_render_result_t* result) ADM_API_NOEXCEPT;

/*
 * Diagnostic log captured during the render that produced `result`.
 * These are the warning/info/debug lines emitted by the engine and renderer
 * backends (importer warnings, semantic-policy rewrites, backend identity,
 * measured loudness/peak, gain adjustments, encoding steps). Use them to drive
 * a log panel or surface non-fatal warnings to the user.
 *
 * NOTE: the terminal failure reason is always available via
 * adm_render_result_message() / adm_render_result_error_code(), even on early
 * errors that abort before any log line is emitted. An empty log does NOT imply
 * the render succeeded — always check the error code.
 */

/* Number of captured log entries. Returns 0 if result is NULL. In the
 * (practically unreachable) event of more than UINT32_MAX entries, the count is
 * saturated at UINT32_MAX rather than truncated. */
uint32_t adm_render_result_log_count(const adm_render_result_t* result) ADM_API_NOEXCEPT;

/*
 * Read the log entry at `index` (0-based, < adm_render_result_log_count).
 * Returns 1 and writes the requested fields if index is valid; returns 0 if
 * result is NULL or index is out of range. Any out pointer may be NULL.
 * The strings written to *out_module / *out_message are owned by the result
 * handle and remain valid until adm_destroy_render_result.
 */
int adm_render_result_log_entry(const adm_render_result_t* result,
                                uint32_t index,
                                adm_log_level_t* out_level,
                                const char** out_module,
                                const char** out_message) ADM_API_NOEXCEPT;

/* ── v1.1 Probe ──────────────────────────────────────────────────────────── */
/*
 * adm_probe_file: quickly import ADM scene metadata without rendering.
 * Returns ADM_ERROR_OK and writes a handle to *out on success.
 * The caller must release the handle with adm_destroy_scene_info.
 * out may be NULL to just validate the file without allocating a handle.
 */
adm_error_code_t
adm_probe_file(adm_context_t* context, const char* input_path, adm_scene_info_t** out) ADM_API_NOEXCEPT;

void adm_destroy_scene_info(adm_scene_info_t* info) ADM_API_NOEXCEPT;

uint32_t adm_scene_info_sample_rate(const adm_scene_info_t* info) ADM_API_NOEXCEPT;
uint32_t adm_scene_info_channels(const adm_scene_info_t* info) ADM_API_NOEXCEPT;
uint64_t adm_scene_info_frames(const adm_scene_info_t* info) ADM_API_NOEXCEPT;
double adm_scene_info_duration_seconds(const adm_scene_info_t* info) ADM_API_NOEXCEPT;
uint32_t adm_scene_info_programme_count(const adm_scene_info_t* info) ADM_API_NOEXCEPT;
uint32_t adm_scene_info_object_count(const adm_scene_info_t* info) ADM_API_NOEXCEPT;

/* ── v1.1 Scene inspect (JSON) ───────────────────────────────────────────── */
/*
 * adm_inspect_file_json: import the full ADM scene and serialize it to a JSON
 * string (UTF-8). The JSON mirrors the `mradm inspect` field set: file info,
 * programmes, contents, objects (with per-track / per-block detail), HOA tracks,
 * and import warnings. Optional fields are omitted when unset.
 *
 * The root object carries a stable schema identity for version detection:
 *   "schema": "mradm.scene-inspect", "schema_version": 1
 * schema_version is bumped only on a breaking change to the field set;
 * additive fields keep version 1.
 *
 * On success returns ADM_ERROR_OK and, if out_json is non-NULL, writes a
 * heap-allocated NUL-terminated string to *out_json. That string is owned by
 * the CALLER and must be released with adm_free_string (never free()/delete).
 * out_json may be NULL to just validate that the file parses, allocating nothing.
 * Returns ADM_ERROR_INVALID_ARGUMENT for a NULL/empty input or context, and the
 * mapped error (e.g. ADM_ERROR_IO) if the file is missing or not valid ADM.
 */
adm_error_code_t
adm_inspect_file_json(adm_context_t* context, const char* input_path, char** out_json) ADM_API_NOEXCEPT;

/*
 * adm_inspect_file_xml: return the raw <axml> chunk (ADM XML) embedded in the
 * BWF file, verbatim (UTF-8). Mirrors `mradm inspect --xml`.
 *
 * On success returns ADM_ERROR_OK and, if out_xml is non-NULL, writes a
 * heap-allocated NUL-terminated string to *out_xml, owned by the CALLER and
 * released with adm_free_string (never free()/delete). out_xml may be NULL to
 * just validate that the chunk can be read, allocating nothing. Returns
 * ADM_ERROR_INVALID_ARGUMENT for a NULL/empty input or context, and the mapped
 * error (e.g. ADM_ERROR_IO) if the file is missing, invalid, or has no axml.
 */
adm_error_code_t adm_inspect_file_xml(adm_context_t* context, const char* input_path, char** out_xml) ADM_API_NOEXCEPT;

/*
 * adm_policy_template_json: build the editable neutral semantic-policy template
 * for the scene as a JSON string (UTF-8). Mirrors
 * `mradm inspect --write-semantic-policy-template` but returns the template
 * in-memory instead of writing a file — a GUI can present/edit it, then feed the
 * edited file back via adm_render_options_set_semantic_policy_path. The result is
 * a valid policy document (root carries "schema": "mradm.semantic-policy.v1").
 *
 * On success returns ADM_ERROR_OK and, if out_json is non-NULL, writes a
 * heap-allocated NUL-terminated string to *out_json, owned by the CALLER and
 * released with adm_free_string. out_json may be NULL to just validate the file.
 * Returns ADM_ERROR_INVALID_ARGUMENT for NULL/empty input or context, and the
 * mapped error (e.g. ADM_ERROR_IO) if the file is missing or not valid ADM.
 */
adm_error_code_t
adm_policy_template_json(adm_context_t* context, const char* input_path, char** out_json) ADM_API_NOEXCEPT;

/* ── v1.13 Semantic write-back (export) ──────────────────────────────────── */
/*
 * adm_export_file: apply the semantic policy carried by opts (in-memory JSON
 * preferred over file path) and write a new ADM BWF at output_path, reusing the
 * source file's PCM and chna chunk byte-for-byte (chunk-level RIFF/BW64 rewrite,
 * no sample decode — bit-exact audio) — only the ADM metadata (axml) is rewritten
 * with the policy-applied values. Pass opts == NULL (or opts with no
 * policy set) for a plain ADM round-trip. Mirrors the `mradm export` subcommand.
 *
 * Stage 1 writes back Objects (object gain/mute; block gain/diffuse/extent/
 * divergence/channelLock/jumpPosition/interpolationLength) and DirectSpeakers
 * gain. Position and HOA pack gain/mute are not written back yet.
 *
 * Returns ADM_ERROR_OK on success, ADM_ERROR_INVALID_ARGUMENT for a NULL/empty
 * context/input/output, and the mapped error (e.g. ADM_ERROR_IO,
 * ADM_ERROR_UNSUPPORTED) on failure.
 */
adm_error_code_t adm_export_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 const adm_render_options_t* opts) ADM_API_NOEXCEPT;

/* Release a heap string returned by adm_inspect_file_json (and any future ABI
 * function documented as returning an owned string). Passing NULL is a safe no-op. */
void adm_free_string(char* s) ADM_API_NOEXCEPT;

/* ── v1.1 Capabilities (JSON) ────────────────────────────────────────────── */
/*
 * adm_capabilities_json: enumerate the available renderer backends and their
 * capabilities as a JSON string (UTF-8). Mirrors the `mradm backends` field set:
 * each backend carries its feature flags (objects / direct_speakers / hoa /
 * channel_lock / object_divergence / screen_ref / diffuse) and a list of
 * supported output layouts. Each backend's "renderer" field is the string a
 * caller passes to adm_render_options_set_renderer ("ear"/"saf"/"hoa"/"saf-binaural").
 *
 * The root object carries a stable schema identity for version detection:
 *   "schema": "mradm.capabilities", "schema_version": 1
 *
 * On success returns ADM_ERROR_OK and writes a heap string to *out_json, owned
 * by the CALLER and released with adm_free_string (never free()/delete).
 * out_json must be non-NULL (returns ADM_ERROR_INVALID_ARGUMENT otherwise),
 * as is context.
 */
adm_error_code_t adm_capabilities_json(adm_context_t* context, char** out_json) ADM_API_NOEXCEPT;

/* ── v1.1 Layouts (JSON) ─────────────────────────────────────────────────── */
/*
 * adm_layouts_json: the output channel-order reference as a JSON string (UTF-8).
 * Mirrors the `mradm layouts` field set: for each container format + layout, the
 * channel count, container mapping description, final channel order, an optional
 * note, and "supported_by" (which renderer backends support that layout —
 * a subset of "ear"/"saf"/"hoa"/"saf-binaural").
 *
 * The root object carries a stable schema identity for version detection:
 *   "schema": "mradm.layouts", "schema_version": 1
 *
 * On success returns ADM_ERROR_OK and writes a heap string to *out_json, owned
 * by the CALLER and released with adm_free_string (never free()/delete).
 * out_json must be non-NULL (returns ADM_ERROR_INVALID_ARGUMENT otherwise),
 * as is context.
 */
adm_error_code_t adm_layouts_json(adm_context_t* context, char** out_json) ADM_API_NOEXCEPT;

/* ── v1.6 Output formats (JSON) ──────────────────────────────────────────── */
/*
 * adm_output_formats_json: the output container-format reference as a JSON string
 * (UTF-8). For each output container the engine can produce, reports its file
 * extensions, whether it is "available" in this build / on this platform (with a
 * human-readable "available_reason" when not), and its constraints: "lossy",
 * "max_channels" (0 = unlimited), "fixed_sample_rate" (0 = any), "supports_height",
 * optional "bit_depths", and a bitrate range ("bitrate_kbps_per_ch" for Opus or
 * "bitrate_kbps_total" for APAC, each {min, max, auto:0}). The root also carries a
 * "features" object of build/platform flags: "apac", "iamf", "iamf_mp4_packager",
 * "sofa". A GUI uses this to gray out unavailable formats and validate inputs,
 * rather than hard-coding the platform/build matrix.
 *
 * The root object carries a stable schema identity for version detection:
 *   "schema": "mradm.output-formats", "schema_version": 1
 *
 * On success returns ADM_ERROR_OK and writes a heap string to *out_json, owned by
 * the CALLER and released with adm_free_string (never free()/delete). out_json
 * must be non-NULL (returns ADM_ERROR_INVALID_ARGUMENT otherwise), as is context.
 *
 * Does no project-file I/O. NOTE: in IAMF-enabled builds, computing the
 * "iamf_mp4_packager" flag probes PATH for an MP4 packager and may briefly spawn a
 * short-lived `mp4box`/`ffmpeg -version` subprocess; in the default build
 * (MR_ADM_ENABLE_IAMF=OFF) that flag is false and no probe runs. If you call this
 * on a hot path, cache the result rather than re-querying.
 */
adm_error_code_t adm_output_formats_json(adm_context_t* context, char** out_json) ADM_API_NOEXCEPT;

/* ── v1.12 Render support matrix (JSON) ─────────────────────────────────── */
/*
 * adm_render_support_matrix_json: concrete renderer × layout × output-target
 * support matrix as a JSON string (UTF-8). This combines the renderer
 * capabilities, layout/channel-order table, and output format/container
 * constraints into entries with:
 *
 *   renderer, layout, layout_id, channels, is_3d,
 *   target, format, container, encoding, supported, optional reason
 *
 * Targets distinguish containers/options that share an extension or codec, e.g.
 * "caf" (PCM CAF) versus "apac_caf", and "iamf" versus "iamf_mp4".
 *
 * The root object carries:
 *   "schema": "mradm.render-support-matrix", "schema_version": 1
 *
 * On success returns ADM_ERROR_OK and writes a heap string to *out_json, owned by
 * the CALLER and released with adm_free_string (never free()/delete). out_json
 * must be non-NULL (returns ADM_ERROR_INVALID_ARGUMENT otherwise), as is context.
 *
 * Does no project-file I/O. Like adm_output_formats_json, IAMF-enabled builds may
 * probe PATH for an MP4 packager while computing the IAMF MP4 availability flag.
 */
adm_error_code_t adm_render_support_matrix_json(adm_context_t* context, char** out_json) ADM_API_NOEXCEPT;

/* ── v1.8 Preview session ────────────────────────────────────────────────── */
/*
 * A reusable preview / scrubbing session. adm_create_preview_session imports the ADM
 * scene and applies the semantic policy from `opts` ONCE; adm_preview_render_window
 * then renders arbitrary output sub-windows of that same (input, options) cheaply by
 * reusing the cached scene (skipping re-import + policy) together with on-demand
 * window rendering. Intended for GUI timeline scrubbing.
 *
 * adm_create_preview_session / adm_destroy_preview_session must be strictly paired.
 * A session is NOT thread-safe; use one per thread or serialize access. The cancel
 * token referenced by `opts` (if any) is captured at creation and applies to every
 * window render. opts may be NULL (all defaults). The render trim fields of opts
 * (set_render_start_sec / set_render_end_sec) are IGNORED — the window is supplied
 * per call to adm_preview_render_window.
 *
 * Returns ADM_ERROR_INVALID_ARGUMENT for NULL context/input/out, the mapped import
 * error (e.g. ADM_ERROR_IO) for a missing/invalid file, or a semantic-policy error.
 * On success writes a session handle to *out.
 */
adm_error_code_t adm_create_preview_session(adm_context_t* context,
                                            const char* input_path,
                                            const adm_render_options_t* opts,
                                            adm_preview_session_t** out) ADM_API_NOEXCEPT;

void adm_destroy_preview_session(adm_preview_session_t* session) ADM_API_NOEXCEPT;

/*
 * Render the output window [start_sec, end_sec) of the session's cached scene to
 * output_path. start_sec must be finite and >= 0. end_sec <= 0 means "to the end"
 * (no tail trim); a positive end_sec must be greater than start_sec (validated at
 * render time). output_path may be NULL to derive a path automatically. progress and
 * result behave as in adm_render_file_ex (result may be NULL). The chosen backend
 * renders only the window when it supports on-demand windowing; the reported
 * loudness / True-Peak describe the window.
 *
 * RESULT ACCESSORS: adm_render_result_output_path / _loudness_lufs / _peak_dbtp and
 * the log accessors work as usual. The semantic report is NOT one of them here: it is
 * built once at session creation from the session's options and is NOT regenerated per
 * window, so adm_render_result_semantic_report_json() on a preview result is always
 * NULL regardless of the opts' capture_semantic_report flag. To inspect the effective
 * policy, render once via adm_render_file_ex with capture enabled (or use
 * adm_inspect_file_json) before/outside the preview loop.
 *
 * Returns ADM_ERROR_INVALID_ARGUMENT for a NULL session or out-of-range start/end.
 */
adm_error_code_t adm_preview_render_window(adm_preview_session_t* session,
                                           double start_sec,
                                           double end_sec,
                                           const char* output_path,
                                           adm_progress_cb progress,
                                           void* user_data,
                                           adm_render_result_t** result) ADM_API_NOEXCEPT;

/* v1.10 structured-progress equivalent of adm_preview_render_window. */
adm_error_code_t adm_preview_render_window_v2(adm_preview_session_t* session,
                                              double start_sec,
                                              double end_sec,
                                              const char* output_path,
                                              adm_progress_v2_cb progress,
                                              void* user_data,
                                              adm_render_result_t** result) ADM_API_NOEXCEPT;

/* ── v1.15 Realtime monitor ───────────────────────────────────────────────── */
/*
 * A persistent realtime monitor: streams the rendered ADM scene to the default audio output
 * device while play / pause / seek / loop control playback. Status, levels and the
 * diagnostics log are POLLED (no callbacks into the caller — see the realtime monitoring
 * design). A monitor is NOT thread-safe; serialize calls or use one per thread.
 *
 * adm_create_monitor imports the scene + applies the semantic policy from `opts`, resolves
 * the backend, and starts the default output device. `opts` may be NULL (all defaults).
 * Backends that require 48 kHz (binaural) reject other input rates. Returns the import /
 * policy / backend error, ADM_ERROR_UNSUPPORTED when the backend has no realtime stream, or
 * ADM_ERROR_INTERNAL when no audio output device is available. create / destroy must pair.
 */
typedef struct adm_monitor_t adm_monitor_t; /* v1.15 */

adm_error_code_t adm_create_monitor(adm_context_t* context,
                                    const char* input_path,
                                    const adm_render_options_t* opts,
                                    adm_monitor_t** out) ADM_API_NOEXCEPT;
void adm_destroy_monitor(adm_monitor_t* monitor) ADM_API_NOEXCEPT;

adm_error_code_t adm_monitor_play(adm_monitor_t* monitor) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_pause(adm_monitor_t* monitor) ADM_API_NOEXCEPT;
adm_error_code_t adm_monitor_seek(adm_monitor_t* monitor, double seconds) ADM_API_NOEXCEPT;
/* end_seconds <= start_seconds disables looping. */
adm_error_code_t
adm_monitor_set_loop(adm_monitor_t* monitor, double start_seconds, double end_seconds) ADM_API_NOEXCEPT;

/*
 * v1.16: a single object's live monitoring override. gain_db is additive on top of the
 * object's baked gain and takes effect on the next rendered block (true realtime). The
 * *_scale fields are the realtime subset of the semantic policy's topology controls
 * (multiplicative, 1.0 = no change): on the binaural backend they take effect via a cheap
 * stream re-prepare; backends that have not yet wired them up (e.g. Apple, which honors
 * only gain) accept the values but ignore them. object_id matches the scene's ADM
 * audioObject id. Set struct_size = sizeof(adm_monitor_override_t) on every element; the
 * library reads array elements using that as the stride, so fields may be appended in a
 * later minor version without breaking callers built against this header.
 */
typedef struct adm_monitor_override_t {
    uint32_t struct_size;
    const char* object_id;
    float gain_db;
    float diffuse_scale;
    float extent_scale; /* legacy/common multiplier for width/height/depth */
    float divergence_scale;
    float extent_width_scale;  /* v1.19: additional width multiplier; default 1.0 when absent */
    float extent_height_scale; /* v1.19: additional height multiplier; default 1.0 when absent */
    float extent_depth_scale;  /* v1.19: additional depth multiplier; default 1.0 when absent */
    /* v1.20: optional DirectSpeakers channel filter. When non-NULL and non-empty, this override
       applies only to the bed channel whose speaker label matches (case/separator-insensitive),
       so one bed (one audioObject, many channels) can be gained per channel. NULL/"" = the whole
       object (default). Mirrors the export semantic-policy DirectSpeakers speaker_label filter. */
    const char* speaker_label;
} adm_monitor_override_t;

/*
 * Replace the full set of live overrides (objects not listed render at their prepared
 * values). `overrides` may be NULL when count == 0 to clear all overrides. Each element's
 * struct_size must cover at least through divergence_scale; the first element's struct_size
 * is the array stride. Non-finite gain_db / *_scale values are rejected with
 * ADM_ERROR_INVALID_ARGUMENT. `revision` is echoed back through
 * adm_monitor_status_t.override_revision once the worker applies the snapshot, so a UI can
 * confirm its edit landed without a callback.
 */
adm_error_code_t adm_monitor_set_overrides(adm_monitor_t* monitor,
                                           const adm_monitor_override_t* overrides,
                                           uint32_t count,
                                           uint64_t revision) ADM_API_NOEXCEPT;

/*
 * v1.17: hot-switch the rendering backend / layout live, reusing the already-imported +
 * policy-applied scene. `opts` selects the new renderer + output layout (other fields as in
 * adm_render_options); NULL uses defaults. The new backend is prepared off the audio thread
 * and crossfaded in. The new stream must run at the current monitor sample rate. A different
 * channel count is folded into the monitor output when the monitor is stereo (multichannel
 * speaker layouts by geometry, HOA by a first-order decode); other channel-count changes
 * return ADM_ERROR_UNSUPPORTED. Returns the resolve / prepare error otherwise.
 */
adm_error_code_t adm_monitor_switch_backend(adm_monitor_t* monitor, const adm_render_options_t* opts) ADM_API_NOEXCEPT;

/* Playback state for adm_monitor_status_t.state. */
typedef enum adm_monitor_state_t {
    ADM_MONITOR_STOPPED = 0,
    ADM_MONITOR_PLAYING = 1,
    ADM_MONITOR_PAUSED = 2
} adm_monitor_state_t;

/*
 * Polled status. Set struct_size = sizeof(adm_monitor_status_t) before the call; the library
 * writes only the fields that fit, so callers built against an older header stay compatible
 * as fields are appended in later minor versions.
 */
typedef struct adm_monitor_status_t {
    uint32_t struct_size;
    int32_t state; /* adm_monitor_state_t */
    uint64_t playhead_frames;
    uint64_t underruns;
    uint64_t buffered_frames;
    float ring_fill;            /* 0..1 ring occupancy */
    int32_t ended;              /* bool: reached end of material */
    int32_t failed;             /* bool: a render error stopped production */
    uint64_t override_revision; /* v1.16: revision of the last applied live overrides */
} adm_monitor_status_t;
adm_error_code_t adm_monitor_get_status(adm_monitor_t* monitor, adm_monitor_status_t* out) ADM_API_NOEXCEPT;

/*
 * Polled per-channel peak / RMS of the most recently played block, plus program loudness
 * (LUFS) of the monitored output. The caller provides peak / rms buffers of `capacity` floats;
 * the library writes min(capacity, channels) values and sets out_count to the actual channel
 * count. peak or rms may be NULL to skip that metric.
 * Set struct_size = sizeof(adm_monitor_levels_t) before the call.
 *
 * v1.18: momentary_lufs / shortterm_lufs / integrated_lufs (ITU-R BS.1770, via libebur128).
 * -inf (HUGE_VALF negated) below the gate / when silent. Only written when struct_size covers
 * their offset, so a v1.17 caller's smaller struct is untouched.
 */
typedef struct adm_monitor_levels_t {
    uint32_t struct_size;
    uint32_t capacity;
    uint32_t out_count;
    float* peak;
    float* rms;
    float momentary_lufs;  /* v1.18: 400 ms window */
    float shortterm_lufs;  /* v1.18: 3 s window */
    float integrated_lufs; /* v1.18: gated, since the last seek */
} adm_monitor_levels_t;
adm_error_code_t adm_monitor_get_levels(adm_monitor_t* monitor, adm_monitor_levels_t* out) ADM_API_NOEXCEPT;

/*
 * Polled diagnostics buffer (append-only for the monitor's lifetime). Read the count, then
 * fetch entries [0, count). out_level receives an adm_log_level_t; the out_module /
 * out_message pointers are owned by the monitor and remain valid only until the next
 * adm_monitor_* call on the same monitor. Returns 1 on success, 0 on a bad index / args.
 */
uint32_t adm_monitor_log_count(adm_monitor_t* monitor) ADM_API_NOEXCEPT;
int adm_monitor_log_entry(adm_monitor_t* monitor,
                          uint32_t index,
                          int32_t* out_level,
                          const char** out_module,
                          const char** out_message) ADM_API_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif
