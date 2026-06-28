#include "capability_json.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/capability.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif
#ifdef _WIN32
#include "audio_output_device.h"
#endif
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

namespace mradm::engine {

namespace {

using nlohmann::json;

json layout_to_json(const CapabilityReport::Layout& layout) {
    json j = json::object();
    j["id"] = layout.id;
    j["display_name"] = layout.display_name;
    j["channel_count"] = layout.channel_count;
    j["is_3d"] = layout.is_3d;
    j["lfe_count"] = layout.lfe_count;
    j["supports_spread"] = layout.supports_spread;
    j["is_binaural"] = layout.is_binaural;
    return j;
}

// `renderer` is the adm_renderer_t / RendererSelection string a consumer would
// pass to set_renderer, so bindings can correlate a backend with that enum.
json backend_to_json(const char* renderer, const CapabilityReport& caps) {
    json j = json::object();
    j["renderer"] = renderer;
    j["backend_name"] = caps.backend_name;
    j["backend_version"] = caps.backend_version;
    j["supports_objects"] = caps.supports_objects;
    j["supports_direct_speakers"] = caps.supports_direct_speakers;
    j["supports_hoa"] = caps.supports_hoa;
    j["supports_channel_lock"] = caps.supports_channel_lock;
    j["supports_object_divergence"] = caps.supports_object_divergence;
    j["supports_screen_ref"] = caps.supports_screen_ref;
    j["supports_diffuse"] = caps.supports_diffuse;
    json layouts = json::array();
    std::ranges::transform(caps.supported_layouts, std::back_inserter(layouts), layout_to_json);
    j["layouts"] = std::move(layouts);
    return j;
}

} // namespace

std::string capabilities_to_json() {
    json root = json::object();
    root["schema"] = "mradm.capabilities";
    root["schema_version"] = 1;

    json backends = json::array();
    // vbap_capabilities() backs RendererSelection::saf.
    backends.push_back(backend_to_json("ear", ear_capabilities()));
    backends.push_back(backend_to_json("saf", vbap_capabilities()));
    backends.push_back(backend_to_json("hoa", hoa_capabilities()));
    backends.push_back(backend_to_json("saf-binaural", binaural_capabilities()));
#ifdef __APPLE__
    backends.push_back(backend_to_json("apple", apple_capabilities()));
#endif
    root["backends"] = std::move(backends);

    // Speaker layouts the platform's system-spatial monitor sink accepts (empty where unsupported).
    // The GUI uses this — not a renderer backend's layouts — as the authoritative list for the
    // "system spatial audio" monitor option, so it works cross-platform without a hardcoded
    // whitelist. macOS: AVSampleBufferAudioRenderer == apple_capabilities()'s non-binaural layouts.
    // Windows: ISpatialAudioClient == adm_windows' windows_layouts table.
    json system_spatial = json::array();
#if defined(__APPLE__)
    // Bind the report to a named local first: range-for over apple_capabilities().supported_layouts
    // directly would dangle (the CapabilityReport temporary dies before the loop body in C++20).
    const CapabilityReport apple_caps = apple_capabilities();
    for (const auto& layout : apple_caps.supported_layouts) {
        if (!layout.is_binaural) {
            json j = json::object();
            j["id"] = layout.id;
            j["display_name"] = layout.display_name;
            j["channel_count"] = layout.channel_count;
            system_spatial.push_back(std::move(j));
        }
    }
#elif defined(_WIN32)
    const std::vector<realtime::SystemSpatialLayoutInfo> win_spatial = realtime::system_spatial_layouts();
    for (const auto& layout : win_spatial) {
        json j = json::object();
        j["id"] = layout.id;
        j["display_name"] = layout.display_name;
        j["channel_count"] = layout.channel_count;
        system_spatial.push_back(std::move(j));
    }
#endif
    root["system_spatial_layouts"] = std::move(system_spatial);

#ifdef __APPLE__
    // Runtime self-test: macOS ≤26.3's AUSpatialMixer misroutes a bypass-LFE source to Center, so an
    // Apple-rendered system-spatial bed loses its LFE. The GUI uses this to steer such systems to an
    // EAR/VBAP bed. Absent on non-macOS → callers treat absence as "ok" (no warning).
    root["apple_system_spatial_lfe_routing_ok"] = apple_system_spatial_lfe_routing_ok();
#endif

    return root.dump(2);
}

} // namespace mradm::engine
