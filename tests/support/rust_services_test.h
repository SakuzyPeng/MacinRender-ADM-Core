// The island tests call the islands' wrappers directly. The C ABI binds the islands' Rust services
// (mradm_islands_bind, src/adm_core/rust_services_ffi.h) on its first guarded call, so each test makes
// one such call before anything else.
#pragma once

#include "adm/c_api.h"

namespace mradm_test {
inline void bind_rust_services() {
    adm_destroy_context(adm_create_context());
}
} // namespace mradm_test
