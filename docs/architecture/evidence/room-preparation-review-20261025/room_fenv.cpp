#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif
#include "adm/render_room.h"
#include "phase2_io.h"
#include "scene_import_internal.h"

namespace {
uint64_t control() {
#if defined(__aarch64__)
    uint64_t value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
#else
    return _mm_getcsr();
#endif
}
void set_control(uint64_t value) {
#if defined(__aarch64__)
    asm volatile("msr fpcr, %0" : : "r"(value));
#else
    _mm_setcsr(static_cast<unsigned>(value));
#endif
}
struct Restore {
    uint64_t saved = control();
    ~Restore() { set_control(saved); }
};
template <class T> T take(mradm::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
std::vector<float> render(mradm::RenderPlan plan, std::size_t count) {
    mradm::NullLogSink logs;
    auto renderer = mradm::create_room_renderer();
    auto prepared = take(renderer->prepare(plan, logs));
    auto stream = take(renderer->open_stream(*prepared, plan, logs));
    std::vector<float> samples(count * 2);
    constexpr std::array<std::size_t, 5> chunks{1, 7, 127, 511, 1024};
    std::size_t done = 0, part = 0;
    while (done < count) {
        const auto frames = std::min(chunks[part++ % chunks.size()], count - done);
        const auto got = take(stream->process(std::span(samples).subspan(done * 2), frames));
        phase2::require(got == frames, "short ROOM stream");
        done += got;
    }
    phase2::require(std::ranges::any_of(samples, [](float x) { return x != 0; }), "silent ROOM probe");
    return samples;
}
}
int main(int argc, char** argv) {
    try {
        phase2::require(argc == 4, "usage: room_fenv input.wav room.json output-dir");
        Restore restore;
        auto scene = take(mradm::io::RenderScene::import(argv[1], nullptr));
        mradm::RenderPlan plan;
        plan.input_path = argv[1]; plan.output_layout = "binaural";
        plan.scene = scene.scene(); plan.scene_handle = scene.handle();
        plan.room.profile = take(mradm::RoomProfile::load(argv[2]));
        plan.room.quality = mradm::RoomQuality::exact;
        const std::filesystem::path out(argv[3]);
        std::filesystem::create_directories(out);
#if defined(__aarch64__)
        constexpr unsigned shift = 22;
        constexpr uint64_t flush_mask = (1ULL << 24) | (1ULL << 19);
        constexpr std::array<unsigned, 4> rounds{0, 1, 2, 3};
#else
        constexpr unsigned shift = 13;
        constexpr uint64_t flush_mask = (1ULL << 15) | (1ULL << 6);
        constexpr std::array<unsigned, 4> rounds{0, 2, 1, 3};
#endif
        const auto mask = (3ULL << shift) | flush_mask;
        std::ofstream manifest(out / "status.json"); manifest << "[";
        for (unsigned mode = 0; mode < 4; ++mode) {
            for (unsigned flush = 0; flush < 2; ++flush) {
                const auto changed = (restore.saved & ~mask) | (uint64_t{rounds[mode]} << shift) |
                                     (flush ? flush_mask : 0);
                for (int pass = 1; pass <= 2; ++pass) {
                    set_control(changed);
                    const auto pcm = render(plan, 4096);
                    phase2::require((control() & mask) == (changed & mask), "caller environment changed");
                    phase2::pcm(out / ("round-" + std::to_string(mode) + "-flush-" + std::to_string(flush) +
                                      "-pass-" + std::to_string(pass) + ".pcmbits"), 2, 48000, pcm);
                }
                if (mode || flush) manifest << ",";
                manifest << "{\"rounding\":" << mode << ",\"flush\":" << flush << ",\"restored\":true}";
            }
        }
        manifest << "]\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 2;
    }
}
