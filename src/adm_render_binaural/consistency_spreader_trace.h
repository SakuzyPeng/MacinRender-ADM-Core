#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
int mr_adm_trace_spreader_instance(void);
void mr_adm_trace_spreader(const char* name, int instance, int frame, const float* values, size_t count);
#ifdef __cplusplus
}
#endif
