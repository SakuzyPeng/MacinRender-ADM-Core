#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "rust_services_ffi.h"

namespace {
std::atomic<const MradmRustServices*> bound{nullptr};
}

extern "C" void mradm_islands_bind(const MradmRustServices* services) {
    bound.store(services, std::memory_order_release);
}

const MradmRustServices& mradm::rust() noexcept {
    const auto* services = bound.load(std::memory_order_acquire);
    if (services == nullptr) {
        std::fputs("mradm: an island ran before Rust bound its services\n", stderr);
        std::abort();
    }
    return *services;
}
