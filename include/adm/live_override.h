#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mradm {

// A single object's live monitoring override, applied on top of the prepared scene
// while a MonitorSession plays. This is the realtime-normalized subset of the
// mradm.semantic-policy.v1 object scope: gain takes effect on the next rendered block
// (a true realtime scalar), while the topology-changing scales (diffuse / extent /
// divergence) are honored by a cheap stream re-prepare on backends that support it
// (binaural today); backends not yet wired up (e.g. Apple, gain only) accept but ignore
// them. See docs/architecture/REALTIME_MONITORING.md §5/§6.
struct LiveObjectOverride {
    std::string object_id;        // SceneObject::id this override applies to
    float gain_db{0.0F};          // additive gain in dB on top of the baked object gain (immediate)
    float diffuse_scale{1.0F};    // multiplies the block diffuse (binaural re-prepare; others ignore)
    float extent_scale{1.0F};     // legacy/common multiplier for width/height/depth
    float divergence_scale{1.0F}; // multiplies divergence (binaural re-prepare; others ignore)
    float extent_width_scale{1.0F};
    float extent_height_scale{1.0F};
    float extent_depth_scale{1.0F};
    // Optional DirectSpeakers channel filter: when non-empty, this override applies only to the
    // bed channel whose speaker label matches (case/separator-insensitive), letting a single bed
    // (one AudioObject, many channels) be gained per channel. Empty = the whole object (default).
    // Mirrors the export-path DirectSpeakersPolicy.speaker_label so live and export stay in sync.
    // Trails the numeric fields so existing positional initializers keep compiling.
    std::string speaker_label;
    // Head-tracking participation. false (default) = world-locked: the object/channel is fixed in
    // the world, so it counter-rotates as the listener turns their head (current behavior). true =
    // head-locked: stays fixed relative to the head (e.g. narration / music), so head tracking does
    // NOT move it. Resolved per channel like gain (whole-object vs per-channel speaker_label). The
    // Apple monitor backend (per-bus head-orientation compensation) and the SAF binaural renderer
    // (per-source direction rotation) honor it; other backends ignore it.
    bool head_locked{false};
};

// The full live-override snapshot handed to a stream. `revision` increments on every
// edit; the engine reports the last applied revision via the status snapshot so a UI
// can confirm its edit landed without a callback.
struct LiveOverrides {
    std::vector<LiveObjectOverride> objects;
    uint64_t revision{0};
};

} // namespace mradm
