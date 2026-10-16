#pragma once
#include <stddef.h>
#include <stdint.h>
// Locale-independent decimal parameters (PCM consistency 3.5); see mradm-dsp `decimal`. Rust's
// correctly rounded parser; `single` != 0 rounds the text once to binary32 (widened exactly).
// Status values follow adm::ErrorCode.
#ifdef __cplusplus
extern "C" {
#endif
int32_t mradm_dsp_parse_decimal(const uint8_t* text, size_t length, uint32_t single, double* value);
#ifdef __cplusplus
}
#endif
