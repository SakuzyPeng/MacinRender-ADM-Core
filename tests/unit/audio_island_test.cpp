// The audio library's render-island entry (mradm_audio_island) as the engine calls it: raw progress,
// the caller's cancellation polled before and while an encoder runs, and the declined islands.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"

#include "../../src/adm_audio/island_ffi.h"

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
    std::atomic<bool> raw{true};
};

uint32_t cancelled(void* user) {
    const auto& c = *static_cast<Caller*>(user);
    return c.cancel_after >= 0 && c.events.load() >= c.cancel_after ? 1U : 0U;
}

void progress(void* user, const MradmEngineProgress* event) {
    auto& c = *static_cast<Caller*>(user);
    // The entry reports the encoder's own events; the engine maps them into the render's range.
    if (event->stage != static_cast<uint32_t>(mradm::RenderStage::post_processing) ||
        event->operation != static_cast<uint32_t>(mradm::RenderOperation::encode_flac)) {
        c.raw = false;
    }
    ++c.events;
}

bool write_source(const fs::path& path, uint64_t frames) {
    auto writer = mradm::audio::FloatWavWriter::open(path.string(), 1U, 48000U);
    if (!writer) {
        return false;
    }
    std::vector<float> block(4096U);
    for (uint64_t done = 0; done < frames; done += block.size()) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<float>(static_cast<int>((done + i) * 37U % 200U) - 100) / 256.0F;
        }
        writer->write(block.data(), block.size());
    }
    return writer->finish().has_value();
}

int32_t run_flac(const fs::path& src, const fs::path& dst, Caller& caller, MradmEngineIslandReply& reply) {
    const auto s = src.string();
    const auto d = dst.string();
    MradmEngineRenderEvent e{};
    e.kind = 3U;
    e.island = 4U;
    e.src.utf8 = view(s);
    e.dst.utf8 = view(d);
    const MradmIslandContext context{&caller, &cancelled, &progress, nullptr};
    return mradm_audio_island(&e, &context, &reply);
}

} // namespace

int main() {
    const auto dir = fs::temp_directory_path() / ("mr_adm_audio_island_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const auto src = dir / "in.wav";
    bool ok = check(write_source(src, uint64_t{48000U} * 30U), "write a 30 s float source");

    Caller plain;
    MradmEngineIslandReply reply{};
    ok &= check(run_flac(src, dir / "plain.flac", plain, reply) == 0 && reply.code == 0, "FLAC island runs");
    ok &= check(plain.events.load() > 2 && plain.raw.load(), "FLAC island reports raw encoder progress");
    ok &= check(fs::file_size(dir / "plain.flac") > 0U, "FLAC island writes the file");

    Caller before;
    before.cancel_after = 0;
    reply = {};
    ok &= check(run_flac(src, dir / "before.flac", before, reply) == 0 &&
                    reply.code == static_cast<int32_t>(mradm::ErrorCode::cancelled),
                "a cancellation before the island is seen at once");

    Caller during;
    during.cancel_after = 1;
    reply = {};
    ok &= check(run_flac(src, dir / "during.flac", during, reply) == 0 &&
                    reply.code == static_cast<int32_t>(mradm::ErrorCode::cancelled),
                "a cancellation while the encoder runs stops it");
    ok &= check(during.events.load() < plain.events.load(), "the cancelled encoder stops early");

    MradmEngineRenderEvent apple{};
    apple.island = 2U;
    const MradmIslandContext none{nullptr, nullptr, nullptr, nullptr};
    ok &= check(mradm_audio_island(&apple, &none, &reply) == 2, "the Apple islands are left to the caller");
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
