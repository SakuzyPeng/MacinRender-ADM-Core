// The audio library's render-island entry (mradm_audio_island) as the engine calls it: it takes only
// APAC. The islands it declines are left to the caller: the Apple backend's, and FLAC, Opus MKA, IAMF,
// the metadata and the MP4 packager, which are in Rust since PCM consistency 7.2 A / B / C (mradm-ffi
// `flac` / `opus_mka` / `iamf` / `mp4_metadata` / `mp4_packager`, tested there), and unknown ones. A
// NULL request is rejected.
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
    for (const uint32_t island : {1U, 4U, 5U, 7U, 8U, 9U}) {
        e.island = island;
        ok &= check(mradm_audio_island(&e, &none, &reply) == 2,
                    "FLAC, Opus MKA, IAMF, metadata and the MP4 packager are left to the caller (Rust)");
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
