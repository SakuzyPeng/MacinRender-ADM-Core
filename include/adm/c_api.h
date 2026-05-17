#pragma once

/*
 * STATUS: experimental.
 * 本 C ABI 在 M3 完成（首个端到端渲染闭环跑通）前不承诺二进制兼容。
 * 升级到任何 0.x.y 版本都可能要求重新编译绑定方。
 * 稳定承诺与版本策略：docs/adr/0007-c-abi-stability-policy.md
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct adm_context_t adm_context_t;
typedef struct adm_render_result_t adm_render_result_t;

typedef enum adm_error_code_t {
    ADM_ERROR_OK = 0,
    ADM_ERROR_INVALID_ARGUMENT = 1,
    ADM_ERROR_UNSUPPORTED = 2,
    ADM_ERROR_IO = 3,
    ADM_ERROR_RENDER_FAILED = 4,
    ADM_ERROR_CANCELLED = 5,
    ADM_ERROR_INTERNAL = 6
} adm_error_code_t;

typedef void (*adm_progress_cb)(double fraction, const char* stage, const char* message, void* user_data);

adm_context_t* adm_create_context(void);
void adm_destroy_context(adm_context_t* context);

adm_error_code_t adm_render_file(adm_context_t* context,
                                 const char* input_path,
                                 const char* output_path,
                                 adm_progress_cb progress,
                                 void* user_data,
                                 adm_render_result_t** result);

void adm_destroy_render_result(adm_render_result_t* result);
adm_error_code_t adm_render_result_error_code(const adm_render_result_t* result);
const char* adm_render_result_message(const adm_render_result_t* result);

#ifdef __cplusplus
}
#endif
