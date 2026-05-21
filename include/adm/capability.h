#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mradm {

struct CapabilityReport {
    struct Layout {
        std::string id;
        std::string display_name;
        uint16_t channel_count{0};   // total output channels (including LFE)
        bool is_3d{false};           // true when any non-LFE speaker has non-zero elevation
        uint16_t lfe_count{0};       // number of LFE channels
        bool supports_spread{false}; // spatial extent spreading available for this layout
    };

    std::string backend_name;
    std::string backend_version;
    std::vector<Layout> supported_layouts;
    bool supports_objects{false};
    bool supports_direct_speakers{false};
    bool supports_hoa{false};
};

} // namespace mradm
