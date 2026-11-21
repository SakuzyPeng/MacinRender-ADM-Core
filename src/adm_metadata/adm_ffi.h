// The borrowed UTF-8 text record of the private FFI (a pointer and a length, not NUL-terminated).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MradmAdmText {
    const uint8_t* data;
    size_t len;
} MradmAdmText;
#ifdef __cplusplus
}
#endif
