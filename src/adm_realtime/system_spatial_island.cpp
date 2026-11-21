#include "system_spatial_island.h"

#include <string>
#include <utility>

namespace mradm::realtime {
namespace {

int32_t open(MradmAdmText layout, MradmDeviceTable* output, const MradmEngineIslandError* e) noexcept {
    if (output == nullptr || e == nullptr) {
        return 1;
    }
    try {
        std::string id = layout.data == nullptr ? std::string{}
                                                : std::string{reinterpret_cast<const char*>(layout.data), layout.len};
        *output = make_avsamplebuffer_device(std::move(id));
        return 0;
    } catch (...) {
        return sink::thrown(e);
    }
}

constexpr MradmSystemSpatialIsland k_island{&open};

} // namespace
} // namespace mradm::realtime

extern "C" const MradmSystemSpatialIsland* mradm_system_spatial_island(void) {
    return &mradm::realtime::k_island;
}
