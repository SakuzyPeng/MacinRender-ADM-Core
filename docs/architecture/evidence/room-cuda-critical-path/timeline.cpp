#include <cuda_profiler_api.h>

#include <nvtx3/nvToolsExt.h>

extern "C" __declspec(dllexport) void room_timeline_event(unsigned stage, unsigned begin) {
    static const char* names[] = {
        "session_process", "canonical_block",     "hrir_query",       "source_response",  "target_filters",
        "emitter_begin",   "emitter_convolution", "worker_roundtrip", "worker_execution", "source_reduction",
        "late_field",      "late_decoder",        "voice_render",     "voice_batch",      "voice_segment",
        "path_transfers",  "response_transform",  "gpu_submit",       "gpu_wait",         "source_eq"};
    if (stage >= 20)
        return;
    if (begin)
        nvtxRangePushA(names[stage]);
    else
        nvtxRangePop();
}
extern "C" __declspec(dllexport) int room_timeline_capture(unsigned begin) {
    return static_cast<int>(begin ? cudaProfilerStart() : cudaProfilerStop());
}
