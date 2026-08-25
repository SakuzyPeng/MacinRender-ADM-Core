#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mradm {

// A single object's live monitoring override, applied on top of the prepared scene
// while a MonitorSession plays. This is the realtime-normalized subset of the
// mradm.semantic-policy.v1 object scope. Gain targets enter a short sample-domain ramp on
// monitoring backends. SAF binaural coalesces diffuse / extent / divergence targets, rebuilds its
// source graph with the prepared HRTF tables, and crossfades the old/new DSP states. See
// docs/architecture/REALTIME_MONITORING.md §5/§6.
struct LiveObjectOverride {
    std::string object_id;        // SceneObject::id this override applies to
    float gain_db{0.0F};          // additive gain target in dB on top of the baked object gain
    float diffuse_scale{1.0F};    // multiplies block diffuse in the SAF binaural source graph
    float extent_scale{1.0F};     // legacy/common multiplier for width/height/depth
    float divergence_scale{1.0F}; // multiplies divergence in the SAF binaural source graph
    float extent_width_scale{1.0F};
    float extent_height_scale{1.0F};
    float extent_depth_scale{1.0F};
    // Optional DirectSpeakers channel filter: when non-empty, this override applies only to the
    // bed channel whose speaker label matches (case/separator-insensitive), letting a single bed
    // (one AudioObject, many channels) be gained per channel. Empty = the whole object (default).
    // Mirrors the export-path DirectSpeakersPolicy.speaker_label so live and export stay in sync.
    // Trails the numeric fields so existing positional initializers keep compiling.
    std::string speaker_label;
    // Explicit live head-lock override. nullopt inherits the active ADM block;
    // false forces scene/world-relative and true forces head-relative. Resolved
    // per channel like gain (whole object vs speaker_label).
    std::optional<bool> head_locked;
    bool mute{false}; // true forces this object/channel to exact silence; gain_db is ignored
};

// The full live-override snapshot handed to a stream. `revision` increments on every
// edit; the engine reports the last applied revision via the status snapshot so a UI
// can confirm its edit landed without a callback.
struct LiveOverrides {
    std::vector<LiveObjectOverride> objects;
    uint64_t revision{0};
};

} // namespace mradm
