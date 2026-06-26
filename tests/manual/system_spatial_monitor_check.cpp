// system_spatial_monitor_check.cpp
//
// Manual real-device check (NOT a ctest assertion): drives a realtime monitor with the
// macOS system-spatial path enabled (adm_render_options_set_monitor_system_spatial), so the
// multichannel render is enqueued into AVSampleBufferAudioRenderer and spatialized by the
// system to the headphone route with head tracking. Put on AirPods, run, and turn your head:
// the soundfield should stay fixed in the world (head tracking working), not glued to the head.
//
//   mr_monitor_spatial_check <input ADM BWF> [output_layout=7.1.4] [seconds=30]
//
// Exercises the real ring-pull path (MonitorEngine worker → ring → ASBR device pull), which
// the standalone spikes did not. Build is gated to macOS; this is a manual tool, not a test.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "adm/c_api.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <input ADM BWF> [output_layout=7.1.4] [seconds=30]\n", argv[0]);
        return 2;
    }
    const char* input = argv[1];
    const char* layout = argc > 2 ? argv[2] : "7.1.4";
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 30;

    adm_context_t* ctx = adm_create_context();
    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_output_layout(opts, layout);
    adm_render_options_set_monitor_system_spatial(opts, 1); // ← the switch under test

    adm_monitor_t* mon = nullptr;
    const adm_error_code_t rc = adm_create_monitor(ctx, input, opts, &mon);
    if (rc != ADM_ERROR_OK || mon == nullptr) {
        std::fprintf(
            stderr,
            "adm_create_monitor 失败: rc=%d（layout=%s 是否受 apple 系统空间化支持？需 5.1…22.2 扬声器布局）\n",
            static_cast<int>(rc),
            layout);
        adm_destroy_monitor(mon);
        adm_destroy_render_options(opts);
        adm_destroy_context(ctx);
        return 1;
    }

    std::printf(
        "⏵ 系统空间化监听: layout=%s, %d 秒。戴 AirPods 转头,听声场是否固定在世界(头追踪生效)。\n", layout, seconds);
    adm_monitor_play(mon);

    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        adm_monitor_status_t st{};
        st.struct_size = sizeof(st);
        if (adm_monitor_get_status(mon, &st) == ADM_ERROR_OK) {
            std::printf("\r  t=%2ds  playhead=%llu  underruns=%llu  ring=%.2f      ",
                        i + 1,
                        static_cast<unsigned long long>(st.playhead_frames),
                        static_cast<unsigned long long>(st.underruns),
                        static_cast<double>(st.ring_fill));
            std::fflush(stdout);
        }
    }
    std::printf("\n");

    adm_destroy_monitor(mon);
    adm_destroy_render_options(opts);
    adm_destroy_context(ctx);
    return 0;
}
