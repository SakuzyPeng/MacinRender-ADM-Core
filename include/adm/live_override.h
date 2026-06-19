#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mradm {

// A single object's live monitoring override, applied on top of the prepared scene
// while a MonitorSession plays. This is the realtime-normalized subset of the
// mradm.semantic-policy.v1 object scope: gain takes effect on the next rendered block
// (a true realtime scalar), while the topology-changing scales (diffuse / extent /
// divergence) are honored by triggering a cheap stream re-prepare + reseek (slice 4).
// See docs/architecture/REALTIME_MONITORING.md §5/§6.
struct LiveObjectOverride {
    std::string object_id;        // SceneObject::id this override applies to
    float gain_db{0.0F};          // additive gain in dB on top of the baked object gain (immediate)
    float diffuse_scale{1.0F};    // multiplies the block diffuse (re-prepare; slice 4)
    float extent_scale{1.0F};     // multiplies width/height/depth (re-prepare; slice 4)
    float divergence_scale{1.0F}; // multiplies divergence (re-prepare; slice 4)
};

// The full live-override snapshot handed to a stream. `revision` increments on every
// edit; the engine reports the last applied revision via the status snapshot so a UI
// can confirm its edit landed without a callback.
struct LiveOverrides {
    std::vector<LiveObjectOverride> objects;
    uint64_t revision{0};
};

} // namespace mradm
