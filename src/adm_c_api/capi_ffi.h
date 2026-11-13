/* The context and output strings of the Rust C ABI (mradm-capi), for the exports still in adm_c_api.cpp. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "adm/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies len bytes (0 clears it). NULL context: OK. Allocation failure: INTERNAL, previous message retained. */
adm_error_code_t mradm_capi_context_set_error(adm_context_t* context, const uint8_t* message, size_t len);

/* A copy of len bytes of text and a terminating NUL, released with adm_free_string; NULL when out of memory. */
char* mradm_capi_string(const uint8_t* text, size_t len);

/* A new owner of the room's package (destroyed with mradm_room_profile_destroy); NULL for a NULL room or OOM. */
void* mradm_capi_room_profile(const adm_room_t* room);

#ifdef __cplusplus
}
#endif
