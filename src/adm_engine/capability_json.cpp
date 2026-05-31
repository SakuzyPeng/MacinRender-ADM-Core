#include "capability_json.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "adm/capability.h"
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
    backends.push_back(backend_to_json("binaural", binaural_capabilities()));
    root["backends"] = std::move(backends);

    return root.dump(2);
}

} // namespace mradm::engine
