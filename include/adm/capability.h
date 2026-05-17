#pragma once

#include <string>
#include <vector>

namespace mradm {

struct CapabilityReport {
    struct Layout {
        std::string id;
        std::string display_name;
    };

    std::string backend_name;
    std::string backend_version;
    std::vector<Layout> supported_layouts;
    bool supports_objects{false};
    bool supports_direct_speakers{false};
    bool supports_hoa{false};
};

} // namespace mradm
