#pragma once

/*
 * MacinRender ADM C ABI — stable v1.
 *
 * STATUS: stable — 自 v1.0.0 起承诺向后二进制兼容。
 * 兼容规则：允许追加新函数、enum 末尾新值、opaque struct 内部变化；
 *            禁止修改现有 signature、enum 已有值、callback 形参。
 *            破坏性变更须发布 v2.0.0 并更新 SONAME。
 * 完整策略：docs/adr/0007-c-abi-stability-policy.md
 */

/* ── Version macros ──────────────────────────────────────────────────────── */

#define ADM_API_VERSION_MAJOR 1
#define ADM_API_VERSION_MINOR 0
#define ADM_API_VERSION_PATCH 0
#define ADM_API_VERSION ((ADM_API_VERSION_MAJOR * 10000) + (ADM_API_VERSION_MINOR * 100) + ADM_API_VERSION_PATCH)

/* ── Deprecation helper ───────────────────────────────────────────────────── */
/*
 * Mark a declaration with ADM_API_DEPRECATED("reason") to signal removal in a
 * future major version. Deprecated symbols are kept for at least 2 minor
 * releases before deletion.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ADM_API_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#define ADM_API_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#define ADM_API_DEPRECATED(msg)
#endif

/* ── Exception boundary ───────────────────────────────────────────────────── */
/*
 * C ABI functions never throw across the boundary. In C++ translation units
 * this is part of the declaration; in C it expands to nothing.
 */
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

/* ABI size guard (C++ translation units only). */
#ifdef __cplusplus
static_assert(sizeof(adm_error_code_t) == sizeof(int), "adm_error_code_t must be int-sized for stable ABI");
#endif

/* ── Progress callback ───────────────────────────────────────────────────── */
/*
 * Called synchronously from within adm_render_file; never invoked after the
 * function returns. stage and message are valid only for the callback's
 * duration. user_data is passed through unchanged.
 */
typedef void (*adm_progress_cb)(double fraction, const char* stage, const char* message, void* user_data);

/* ── Runtime version query ───────────────────────────────────────────────── */
/*
 * Use when loading the library dynamically (dlopen) to verify compatibility
 * before calling other functions. Return values match ADM_API_VERSION_*.
 */
int adm_api_version_major(void) ADM_API_NOEXCEPT;
int adm_api_version_minor(void) ADM_API_NOEXCEPT;
int adm_api_version_patch(void) ADM_API_NOEXCEPT;

/* ── Context lifecycle ───────────────────────────────────────────────────── */
/*
 * adm_create_context / adm_destroy_context must be strictly paired; passing a
 * context to adm_destroy_context more than once is undefined behaviour.
 * A single context is not thread-safe; use one context per thread or
 * serialize access externally.
 */
adm_context_t* adm_create_context(void) ADM_API_NOEXCEPT;
void adm_destroy_context(adm_context_t* context) ADM_API_NOEXCEPT;

/* ── Render ──────────────────────────────────────────────────────────────── */
/*
 * Render input_path to output_path. output_path may be NULL to use an
 * automatically derived path (stem + "_rendered.wav" in the same directory).
 * progress may be NULL. If result is non-NULL, *result receives a handle
 * that the caller must release with adm_destroy_render_result; passing
 * result == NULL suppresses result allocation entirely.
 */
adm_error_code_t adm_render_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 adm_progress_cb progress,
                                 void* user_data,
                                 adm_render_result_t** result) ADM_API_NOEXCEPT;

/* ── Result lifecycle ────────────────────────────────────────────────────── */
/*
 * Call adm_destroy_render_result for every non-NULL result handle.
 * Strings returned by adm_render_result_message are owned by the result
 * handle and remain valid until adm_destroy_render_result is called;
 * do not free them.
 */
void adm_destroy_render_result(adm_render_result_t* result) ADM_API_NOEXCEPT;
adm_error_code_t adm_render_result_error_code(const adm_render_result_t* result) ADM_API_NOEXCEPT;
const char* adm_render_result_message(const adm_render_result_t* result) ADM_API_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif
