#include "consistency_spreader_trace.h"

#include <atomic>

#include "consistency_trace.h"

int mr_adm_trace_spreader_instance(void) {
    static std::atomic<int> next{0};
    return next.fetch_add(1);
}

void mr_adm_trace_spreader(const char* name, int instance, int frame, const float* values, size_t count) {
    std::string path = "spreader-" + std::to_string(instance) + "/";
    if (frame >= 0) {
        path += "frame-" + std::to_string(frame) + "/";
    }
    mradm::consistency::dump(path + name, std::span<const float>(values, count));
}
