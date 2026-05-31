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
 *   adm_layouts_json.
 */

/* ── Version macros ──────────────────────────────────────────────────────── */

#define ADM_API_VERSION_MAJOR 1
#define ADM_API_VERSION_MINOR 1
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
typedef struct adm_render_options_t adm_render_options_t; /* v1.1 */
typedef struct adm_scene_info_t adm_scene_info_t;         /* v1.1 */

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
    ADM_RENDERER_BINAURAL = 5
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

/* Severity of a captured diagnostic log entry (see adm_render_result_log_entry). */
typedef enum adm_log_level_t {
    ADM_LOG_DEBUG = 0,
    ADM_LOG_INFO = 1,
    ADM_LOG_WARNING = 2,
    ADM_LOG_ERROR = 3
} adm_log_level_t;

#ifdef __cplusplus
static_assert(sizeof(adm_renderer_t) == sizeof(int));
static_assert(sizeof(adm_output_bit_depth_t) == sizeof(int));
static_assert(sizeof(adm_speaker_spread_mode_t) == sizeof(int));
static_assert(sizeof(adm_binaural_spread_mode_t) == sizeof(int));
static_assert(sizeof(adm_iamf_container_t) == sizeof(int));
static_assert(sizeof(adm_log_level_t) == sizeof(int));
#endif

/* ── Progress callback ───────────────────────────────────────────────────── */
/*
 * Called synchronously from within adm_render_file / adm_render_file_ex;
 * never invoked after the function returns. stage and message are valid only
 * for the callback's duration. user_data is passed through unchanged.
 */
typedef void (*adm_progress_cb)(double fraction, const char* stage, const char* message, void* user_data);

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

/* APAC total target/hint bitrate in kbps (macOS only). 0 = layout default; otherwise [64, 12000].
 * Returns ADM_ERROR_INVALID_ARGUMENT if kbps is outside [64, 12000] and not 0. */
adm_error_code_t adm_render_options_set_apac_bitrate_kbps(adm_render_options_t* opts, uint32_t kbps) ADM_API_NOEXCEPT;

/* enabled: 1 = Music DRC profile (default), 0 = None. */
void adm_render_options_set_apac_drc_music(adm_render_options_t* opts, int enabled) ADM_API_NOEXCEPT;

/* sofa_path: user SOFA HRIR file path for binaural rendering. NULL or "" = built-in KEMAR.
 * May return ADM_ERROR_INTERNAL on OOM. */
adm_error_code_t adm_render_options_set_sofa_path(adm_render_options_t* opts, const char* sofa_path) ADM_API_NOEXCEPT;

/* semantic_policy_path / semantic_report_path: NULL or "" clears the field.
 * May return ADM_ERROR_INTERNAL on OOM. */
adm_error_code_t adm_render_options_set_semantic_policy_path(adm_render_options_t* opts,
                                                             const char* path) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_options_set_semantic_report_path(adm_render_options_t* opts,
                                                             const char* path) ADM_API_NOEXCEPT;

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
 * caller passes to adm_render_options_set_renderer ("ear"/"saf"/"hoa"/"binaural").
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
 * a subset of "ear"/"saf"/"hoa"/"binaural").
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

#ifdef __cplusplus
} /* extern "C" */
#endif
