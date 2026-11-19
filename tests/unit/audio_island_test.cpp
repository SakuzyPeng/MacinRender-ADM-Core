// The audio library's render-island entry (mradm_audio_island) as the engine calls it: the caller's
// cancellation seen before an encoder runs, and the declined islands (the Apple backend's, FLAC, which
// is in Rust since PCM consistency 7.2 A, and unknown ones).
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adm/audio_io.h"

#include "../../src/adm_audio/island_ffi.h"
#include "../support/rust_services_test.h"

namespace {

namespace fs = std::filesystem;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

MradmAdmText view(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// A caller: counts progress events and cancels after `cancel_after` of them (0: from the start,
// negative: never).
struct Caller {
    int cancel_after{-1};
    std::atomic<int> events{0};
};

uint32_t cancelled(void* user) {
    const auto& c = *static_cast<Caller*>(user);
    return c.cancel_after >= 0 && c.events.load() >= c.cancel_after ? 1U : 0U;
}

void progress(void* user, const MradmEngineProgress* /*event*/) {
    ++static_cast<Caller*>(user)->events;
}

int32_t run_encoder(uint32_t island,
                    const fs::path& src,
                    const fs::path& dst,
                    std::string_view layout,
                    Caller& caller,
                    MradmEngineIslandReply& reply) {
    const auto src_utf8 = src.u8string();
    const auto dst_utf8 = dst.u8string();
    MradmEngineRenderEvent e{};
    e.kind = 3U;
    e.island = island;
    e.src.utf8 = {reinterpret_cast<const uint8_t*>(src_utf8.data()), src_utf8.size()};
    e.dst.utf8 = {reinterpret_cast<const uint8_t*>(dst_utf8.data()), dst_utf8.size()};
    e.layout = view(layout);
    const MradmIslandContext context{&caller, &cancelled, &progress, nullptr};
    return mradm_audio_island(&e, &context, &reply);
}

// A caller that has already cancelled: the Opus encoder returns at once and leaves no output and the
// source in place (moved from trim_file_smoke_test, R3 F1; the FLAC encoder's is a Rust test).
bool verify_pre_cancelled_encoders(const fs::path& dir) {
    const auto src = dir / "stereo.wav";
    {
        auto writer = mradm::audio::FloatWavWriter::open(src.string(), 2U, 48000U);
        if (!writer) {
            return check(false, "write a stereo source");
        }
        const std::vector<float> block(512, 0.25F);
        if (writer->write(block.data(), 256) != 256U || !writer->finish()) {
            return check(false, "finish the stereo source");
        }
    }
    bool ok = true;
    for (const auto& [island, name] : {std::pair<uint32_t, const char*>{5U, "pre.mka"}}) {
        Caller before;
        before.cancel_after = 0;
        MradmEngineIslandReply reply{};
        const auto dst = dir / name;
        ok &= check(run_encoder(island, src, dst, "0+2+0", before, reply) == 0 &&
                        reply.code == static_cast<int32_t>(mradm::ErrorCode::cancelled),
                    "a pre-cancelled encoder returns cancelled");
        ok &= check(!fs::exists(dst), "a pre-cancelled encoder leaves no output");
    }
    return check(fs::exists(src), "a pre-cancelled encoder keeps the source") && ok;
}

} // namespace

int main() {
    mradm_test::bind_rust_services();
    // These native path bytes name different characters in UTF-8 and legacy Windows code pages.
    const auto dir =
        fs::temp_directory_path() / ("mr_adm_audio_island_\xC2\xA9_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    bool ok = verify_pre_cancelled_encoders(dir);

    MradmEngineIslandReply reply{};
    MradmEngineRenderEvent apple{};
    apple.island = 2U;
    const MradmIslandContext none{nullptr, nullptr, nullptr, nullptr};
    ok &= check(mradm_audio_island(&apple, &none, &reply) == 2, "the Apple islands are left to the caller");
    apple.island = 4U;
    ok &= check(mradm_audio_island(&apple, &none, &reply) == 2, "FLAC is left to the caller (Rust)");
    apple.island = 10U;
    ok &= check(mradm_audio_island(&apple, &none, &reply) == 2, "unknown islands are left to the caller");
    ok &= check(mradm_audio_island(nullptr, &none, &reply) == 1, "NULL request is rejected");

    std::error_code ignored;
    fs::remove_all(dir, ignored);
    if (!ok) {
        return 1;
    }
    std::cout << "audio island tests passed\n";
    return 0;
}
