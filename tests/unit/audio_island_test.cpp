// The audio library's render-island entry (mradm_audio_island) as the engine calls it: the islands it
// declines are left to the caller (the Apple backend's, FLAC and Opus MKA, which are in Rust since PCM
// consistency 7.2 A / B, and unknown ones), and a NULL request is rejected. The encoders' cancellation
// and progress are tested next to their Rust implementations (mradm-ffi `flac` / `opus_mka`).
#include <cstdint>
#include <iostream>
#include <string_view>

#include "../../src/adm_audio/island_ffi.h"
#include "../support/rust_services_test.h"

namespace {

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    mradm_test::bind_rust_services();
    bool ok = true;
    MradmEngineIslandReply reply{};
    MradmEngineRenderEvent e{};
    const MradmIslandContext none{nullptr, nullptr, nullptr, nullptr};
    for (const uint32_t island : {2U, 3U}) {
        e.island = island;
        ok &= check(mradm_audio_island(&e, &none, &reply) == 2, "the Apple islands are left to the caller");
    }
    for (const uint32_t island : {4U, 5U}) {
        e.island = island;
        ok &= check(mradm_audio_island(&e, &none, &reply) == 2, "FLAC and Opus MKA are left to the caller (Rust)");
    }
    for (const uint32_t island : {0U, 10U}) {
        e.island = island;
        ok &= check(mradm_audio_island(&e, &none, &reply) == 2, "unknown islands are left to the caller");
    }
    ok &= check(mradm_audio_island(nullptr, &none, &reply) == 1, "NULL request is rejected");
    if (!ok) {
        return 1;
    }
    std::cout << "audio island tests passed\n";
    return 0;
}
