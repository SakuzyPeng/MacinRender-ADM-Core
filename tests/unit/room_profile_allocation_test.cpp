#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

#include "room_internal.h"

namespace {
std::atomic<int> allocations_until_failure{0}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}

// Replace only C++ allocations: resource loading through Rust is completed before arming the fault.
void* operator new(std::size_t size) {
    if (allocations_until_failure.load() > 0 && allocations_until_failure.fetch_sub(1) == 1) {
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) { // NOLINT(cppcoreguidelines-no-malloc)
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc)
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc)
}

namespace {
void* load_profile(const std::string& path) {
    void* profile = nullptr;
    std::array<char, 512> message{};
    if (mradm_room_profile_load(path.data(), path.size(), &profile, message.data(), message.size()) != 0) {
        throw std::runtime_error{message.data()};
    }
    return profile;
}

void allocation_failure(const std::string& path, int allocation) {
    auto* raw = load_profile(path);
    allocations_until_failure = allocation;
    bool caught = false;
    try {
        const auto profile = mradm::room_internal::Access::adopt(raw);
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    const int pending = allocations_until_failure.exchange(0);
    if (!caught || pending != 0) {
        throw std::runtime_error{"room owner allocation did not fail at the requested allocation"};
    }
    // Both the wrapper-allocation and shared_ptr-control-block failures must release safely.
    // A fresh adoption after either failure must still expose the loaded package.
    const auto retry = mradm::room_internal::Access::adopt(load_profile(path));
    if (retry->manifest_json().empty()) {
        throw std::runtime_error{"room profile retry lost its manifest"};
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    try {
        allocation_failure(argv[1], 1);
        allocation_failure(argv[1], 2);
        if (mradm::room_internal::Access::adopt(nullptr)) {
            return 1;
        }
        std::puts("room profile allocation recovery: ok");
        return 0;
    } catch (const std::exception& error) {
        allocations_until_failure = 0;
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
