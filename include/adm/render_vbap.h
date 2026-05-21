#pragma once

#include <memory>
#include <string>
#include <vector>

#include "adm/render.h"

namespace mradm {

// Speaker position descriptor used to define custom VBAP output layouts.
struct VbapSpeakerSpec {
    float azimuth{0.0F};   // degrees, +ve = left (BS.2051 convention)
    float elevation{0.0F}; // degrees, +ve = up
    std::string label;     // BS.2051 label, e.g. "M+030"; empty for unlabelled
    bool is_lfe{false};    // LFE channel — not panned, routed by label only
};

// Register a custom VBAP output layout.  Must be called before any render().
// Returns false if the id is already registered (built-in layouts cannot be
// overridden; custom layouts can be registered only once per id).
bool register_vbap_layout(std::string id, std::string display_name, std::vector<VbapSpeakerSpec> speakers);

CapabilityReport vbap_capabilities();
std::unique_ptr<IRenderer> create_vbap_renderer();

} // namespace mradm
