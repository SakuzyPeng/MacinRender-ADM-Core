#pragma once

// The C++ routing and object-preprocessing helpers over AdmScene blocks (moved from
// src/adm_render_common/render_common.h in PCM consistency 5.6 C): the rules are Rust
// (mradm-render `routing` / `scene`); the C++ tests and the frozen references call them here.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "adm/direct_speakers_matrix.h"
#include "adm/logging.h"
#include "adm/render.h"
#include "adm/scene.h"

#include "../render_test_ffi.h"
#include "adm_scene.h"
#include "render_common.h"

namespace mradm::render_common {

// Blocks of one track sharing a start: the last one in scene (ADM document) order is in effect
// for its entire interval, even when it ends before its peers. Resolve this before a backend
// splits blocks into lanes (point / cloud / diffuse / spreader sources, LFE or divergence buses)
// or interpolates from a predecessor, so an overwritten block leaves no lane, ramp or routing
// decision behind. The result is ordered by start (stable for equal starts before the collapse).
template <typename Block>
[[nodiscard]] std::vector<const Block*> last_blocks_at_each_start(const std::vector<Block>& blocks) {
    std::vector<const Block*> ordered;
    ordered.reserve(blocks.size());
    std::ranges::transform(blocks, std::back_inserter(ordered), [](const auto& block) { return &block; });
    std::ranges::stable_sort(ordered, {}, &Block::start_sample);
    std::size_t count = 0;
    for (const auto* block : ordered) {
        if (count > 0 && ordered[count - 1]->start_sample == block->start_sample) {
            ordered[count - 1] = block;
        } else {
            ordered[count++] = block;
        }
    }
    ordered.resize(count);
    return ordered;
}

// Canonical speaker-label key for per-channel matching: keep alphanumerics AND '+' / '-',
// uppercased. Unlike the alphanumeric key this PRESERVES the sign, so "M+030" and "M-030"
// (left vs right) stay distinct while "m+030" / "M+030" / "M_+030" compare equal. Used for the
// per-channel live-gain match (channel key + override speaker_label canonicalised the same way).
[[nodiscard]] std::string canonicalise_speaker_label(std::string_view raw);

// Canonical label used by matrix source/target matching. In addition to the
// punctuation/case normalisation above, known RoomCentric / DAW aliases are
// converted to their BS.2051 label (for example L -> M+030).
[[nodiscard]] std::string canonical_direct_speaker_label(std::string_view raw);

[[nodiscard]] std::string_view direct_speakers_routing_mode_name(DirectSpeakersRoutingMode mode) noexcept;

// Backend-neutral output slot used by shared DirectSpeakers label/fallback
// routing. Backends adapt their native layout representation into this view.
struct DirectSpeakerRoutingTarget {
    std::string_view label;
    float azimuth{0.0F};
    float elevation{0.0F};
    bool is_lfe{false};
};

// Resolve a DirectSpeakers label in two passes: first exact output-label
// matching, then the project's established RoomCentric/DAW alias table.
[[nodiscard]] std::optional<std::size_t>
direct_speaker_index_for_labels(std::span<const DirectSpeakerRoutingTarget> targets,
                                const std::vector<std::string>& labels);

struct ResolvedDirectSpeakersMatrixRoute {
    std::string source_label;
    std::string source_key;
    bool mute{false};
    std::vector<ResolvedDirectSpeakersMatrixTarget> targets;
};

struct ResolvedDirectSpeakersMatrix {
    std::vector<ResolvedDirectSpeakersMatrixRoute> routes;
    std::shared_ptr<MradmRenderMatrix> handle; // the Rust resolution `routes` copies
};

// Resolve every matrix target label to a concrete output slot. Unknown labels,
// alias-colliding duplicates, LFE rows/targets, and duplicate source rows fail.
[[nodiscard]] Result<ResolvedDirectSpeakersMatrix>
resolve_direct_speakers_matrix_targets(const DirectSpeakersMatrix& matrix,
                                       std::span<const DirectSpeakerRoutingTarget> targets,
                                       std::string_view layout_id);

// Match one non-LFE ADM block to exactly one resolved source row.
[[nodiscard]] Result<const ResolvedDirectSpeakersMatrixRoute*>
direct_speakers_matrix_route_for_block(const ResolvedDirectSpeakersMatrix& matrix,
                                       const SceneDirectSpeakersBlock& block);

struct DirectSpeakerPosition {
    float azimuth{0.0F};
    float elevation{0.0F};
};

// Recover a nominal direction from a known BS.2051 label or one of the shared
// RoomCentric/DAW aliases. This is used when label routing cannot find the same
// slot in the target layout (for example M+135 rendered to a 5.1 M+110 layout).
// Middle/upper/bottom/top layers use nominal elevations 0/+30/-30/+90 degrees.
[[nodiscard]] std::optional<DirectSpeakerPosition>
direct_speaker_position_for_labels(const std::vector<std::string>& labels);

// Read a DirectSpeakers block's nominal coordinates. Missing coordinates use
// front-centre (0, 0) and emit the shared warning required by both backends.
[[nodiscard]] DirectSpeakerPosition
direct_speaker_position_or_front(const SceneDirectSpeakersBlock& block, LogSink& logs, std::string_view log_module);

// ADM producers are inconsistent: some LFE DirectSpeakers channels carry
// channelFrequency lowPass, while others only encode the role in labels like
// RC_LFE/RCLFE/LFE1. Use this helper before positional fallback/spatialization.
[[nodiscard]] bool is_lfe_label(std::string_view raw) noexcept;
[[nodiscard]] bool direct_speakers_block_is_lfe(const SceneDirectSpeakersBlock& block) noexcept;

[[nodiscard]] LfeTarget direct_speakers_lfe_target(const SceneDirectSpeakersBlock& block) noexcept;

[[nodiscard]] Result<LfeRoutingPlan>
resolve_lfe_routing(const ScenePlan& plan, LogSink& logs, std::string_view log_module);

struct PreparedObjectBlock {
    std::vector<SceneObjectBlock> sources;
    uint64_t start_sample{0};
    uint64_t end_sample{0};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

[[nodiscard]] PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                                       const SceneObject& object,
                                                       const std::vector<SceneOutputSpeaker>& speakers,
                                                       LogSink& logs,
                                                       std::string_view log_module,
                                                       bool& screen_ref_warned);

// One direction of the 17-point "disk cloud" used to approximate ADM object extent
// (width/height/depth) as a set of coherent point sources. azimuth/elevation are in the
// project polar convention (azimuth +ve = left, BS.2051). weight is a linear gain weight: a
// linear partition of unity (the active directions' weights sum to 1) so the cloud preserves
// the source's total gain.
struct ExtentDirection {
    float azimuth{0.0F};
    float elevation{0.0F};
    float weight{1.0F}; // linear gain weight
};

// Expand an object block position + extent (width/height/depth, each 0..1) into the
// 17-point disk cloud. When the effective extent is ~0 a single centre direction
// (weight 1) is returned, so a point object stays a point. Otherwise the 16 ring
// directions are returned (inner/outer rings, weights summing to 1), mapped around the
// position via a tangent frame with tan() angular scaling. Pure; the radius mapping
// (width*60, height*45, depth*20, distance-based spread scaling) matches the binaural /
// HOA extent clouds.
[[nodiscard]] std::vector<ExtentDirection>
extent_disk_cloud(const SceneBlockPosition& position, float width, float height, float depth);

} // namespace mradm::render_common
