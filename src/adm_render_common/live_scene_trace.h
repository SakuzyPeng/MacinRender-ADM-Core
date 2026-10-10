#pragma once

// Opt-in Scene checkpoints. Normal builds compile these calls away. States are written as two
// word streams so producer and effective state can be compared bit for bit: every float field
// in a fixed order, plus the integer metadata (role of the row, element id, offsets, masks).
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "consistency_trace.h"
#include "live_scene_renderer.h"

namespace mradm::consistency {

#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
namespace detail {

[[nodiscard]] std::string_view renderer_role() noexcept;
std::string_view set_renderer_role(std::string_view role) noexcept;

inline int low_word(std::uint64_t value) {
    return static_cast<int>(static_cast<std::uint32_t>(value));
}
inline int high_word(std::uint64_t value) {
    return static_cast<int>(static_cast<std::uint32_t>(value >> 32U));
}

inline void append_state(std::vector<float>& values, std::vector<int>& fields, const live_scene::ObjectState& s) {
    values.insert(values.end(),
                  {s.linear_gain,
                   s.x,
                   s.y,
                   s.z,
                   s.width,
                   s.height,
                   s.depth,
                   s.diffuse,
                   s.divergence,
                   s.divergence_azimuth_range,
                   s.divergence_position_range,
                   s.channel_lock_max_distance.value_or(0.0F)});
    fields.insert(fields.end(),
                  {low_word(s.valid_fields),
                   s.active ? 1 : 0,
                   s.channel_lock ? 1 : 0,
                   s.screen_reference ? 1 : 0,
                   s.head_locked ? 1 : 0,
                   s.channel_lock_max_distance.has_value() ? 1 : 0});
}

} // namespace detail

// Both renderers receive the same Frame during a crossfade, including a same-backend
// SOFA switch. Scope their checkpoints by role without changing the production Frame ABI.
class SceneRendererTraceScope {
  public:
    explicit SceneRendererTraceScope(std::string_view role) noexcept : previous_(detail::set_renderer_role(role)) {}
    ~SceneRendererTraceScope() { detail::set_renderer_role(previous_); }
    SceneRendererTraceScope(const SceneRendererTraceScope&) = delete;
    SceneRendererTraceScope& operator=(const SceneRendererTraceScope&) = delete;
    SceneRendererTraceScope(SceneRendererTraceScope&&) = delete;
    SceneRendererTraceScope& operator=(SceneRendererTraceScope&&) = delete;

  private:
    std::string_view previous_;
};
#endif

// Writes `<stage>.f32` (12 floats per row) and `<stage>-fields.i32` (rows of 15 words:
// kind 0 = initial state / 1 = update, element id low/high, offset, ramp, jump, changed,
// cleared, stream order, then the six state flags). Nothing is written for an empty slice.
inline void dump_states(const std::string& stage,
                        std::span<const live_scene::StateEntry> initial,
                        std::span<const live_scene::MetadataUpdate> updates) {
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    std::vector<float> values;
    std::vector<int> fields;
    for (const auto& entry : initial) {
        fields.insert(fields.end(),
                      {0, detail::low_word(entry.element_id), detail::high_word(entry.element_id), 0, 0, 0, 0, 0, 0});
        detail::append_state(values, fields, entry.state);
    }
    for (const auto& update : updates) {
        fields.insert(fields.end(),
                      {1,
                       detail::low_word(update.element_id),
                       detail::high_word(update.element_id),
                       static_cast<int>(update.offset_samples),
                       static_cast<int>(update.ramp_duration_samples),
                       update.jump_position ? 1 : 0,
                       detail::low_word(update.changed_fields),
                       detail::low_word(update.cleared_fields),
                       detail::low_word(update.stream_order)});
        detail::append_state(values, fields, update.state);
    }
    if (!values.empty()) {
        dump(stage + ".f32", values);
        dump(stage + "-fields.i32", fields);
    }
#else
    (void) stage;
    (void) initial;
    (void) updates;
#endif
}

// Prefix shared by every checkpoint of one rendered Scene slice.
inline std::string scene_slice_key(const live_scene::Frame& frame) {
    return "scene/e" + std::to_string(frame.epoch_id) + "-g" + std::to_string(frame.generation_id) + "-s" +
           std::to_string(frame.media_sample_start);
}

#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
inline std::string scene_renderer_key(const live_scene::Frame& frame) {
    return scene_slice_key(frame) + "-" + std::string{detail::renderer_role()};
}
#endif

} // namespace mradm::consistency
