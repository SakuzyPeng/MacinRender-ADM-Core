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
        bool is_binaural{false};     // true only when HRTF convolution is applied
    };

    std::string backend_name;
    std::string backend_version;
    std::vector<Layout> supported_layouts;
    bool supports_objects{false};
    bool supports_direct_speakers{false};
    bool supports_hoa{false};
    bool supports_channel_lock{false};
    bool supports_object_divergence{false};
    bool supports_screen_ref{false};
    bool supports_diffuse{false};
    // Internal optimization hint (not surfaced in capabilities JSON): the backend
    // honors RenderPlan::render_window — it can render only the requested output
    // sub-window using an internal warm-up pre-roll, bit-identical to a full render
    // then trimmed. When false, RenderService renders the full timeline and trims
    // the file afterward.
    bool supports_render_window{false};
};

} // namespace mradm
