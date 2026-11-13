// The room screen geometry record of Rust mradm-dsp `room::loading`. The room entries only tests
// call are in tests/support/ffi/room_test_ffi.h.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// cppcheck-suppress-begin unusedStructMember
typedef struct MradmRoomScreenGeometry {
    uint32_t cartesian;
    uint32_t reserved;
    double aspect_ratio;
    double centre[3];
    double width;
} MradmRoomScreenGeometry;
// cppcheck-suppress-end unusedStructMember
#ifdef __cplusplus
}
#endif
