#include "live_triple_balance_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bed.h"
#include "live_scene_trace.h"
#include "live_triple_balance.h"
#include "render_common.h"
#include "triple_balance.h"

namespace mradm::live_scene {
namespace {
struct RuntimeElement {
    ElementDescriptor descriptor;
    ObjectState target_state;
    bool initialized{false};
};

[[nodiscard]] float level_for(const ObjectState& state) noexcept {
    if ((state.valid_fields & state_active) != 0U && !state.active) {
        return 0.0F;
    }
    return (state.valid_fields & state_linear_gain) != 0U ? state.linear_gain : 1.0F;
}

[[nodiscard]] ObjectState default_state(const ElementDescriptor& descriptor) {
    ObjectState state;
    state.valid_fields = k_known_state_fields & ~state_position;
    if (descriptor.role == ElementRole::object) {
        state.valid_fields |= state_position;
    }
    if (descriptor.has_position) {
        state.x = descriptor.x;
        state.y = descriptor.y;
        state.z = descriptor.z;
    }
    return state;
}

void copy_state_fields(ObjectState& destination, const ObjectState& source, std::uint64_t requested_fields) {
    const auto fields = source.valid_fields & requested_fields;
    if ((fields & state_active) != 0U) {
        destination.active = source.active;
    }
    if ((fields & state_linear_gain) != 0U) {
        destination.linear_gain = source.linear_gain;
    }
    if ((fields & state_position) != 0U) {
        destination.x = source.x;
        destination.y = source.y;
        destination.z = source.z;
    }
    if ((fields & state_extent) != 0U) {
        destination.width = source.width;
        destination.height = source.height;
        destination.depth = source.depth;
    }
    if ((fields & state_diffuse) != 0U) {
        destination.diffuse = source.diffuse;
    }
    if ((fields & state_divergence) != 0U) {
        destination.divergence = source.divergence;
    }
    if ((fields & state_channel_lock) != 0U) {
        destination.channel_lock = source.channel_lock;
    }
    if ((fields & state_screen_reference) != 0U) {
        destination.screen_reference = source.screen_reference;
    }
    if ((fields & state_head_locked) != 0U) {
        destination.head_locked = source.head_locked;
    }
    if ((fields & state_divergence_range) != 0U) {
        destination.divergence_azimuth_range = source.divergence_azimuth_range;
        destination.divergence_position_range = source.divergence_position_range;
    }
    if ((fields & state_channel_lock_max_distance) != 0U) {
        destination.channel_lock_max_distance = source.channel_lock_max_distance;
    }
    destination.valid_fields |= fields;
}

// Producer ranges are validated upstream. Policy-expanded coordinates/extents may exceed them;
// this numerical boundary checks finiteness without narrowing the policy domain.
[[nodiscard]] bool valid_state(const ObjectState& state) noexcept {
    const auto coordinate = [](float v) { return std::isfinite(v); };
    const auto finite = [](float v) { return std::isfinite(v); };
    if ((state.valid_fields & ~k_known_state_fields) != 0U) {
        return false;
    }
    if ((state.valid_fields & state_linear_gain) != 0U &&
        (!std::isfinite(state.linear_gain) || state.linear_gain < 0)) {
        return false;
    }
    if ((state.valid_fields & state_position) != 0U &&
        (!coordinate(state.x) || !coordinate(state.y) || !coordinate(state.z))) {
        return false;
    }
    if ((state.valid_fields & state_extent) != 0U &&
        (!finite(state.width) || !finite(state.height) || !finite(state.depth))) {
        return false;
    }
    if ((state.valid_fields & state_diffuse) != 0U && !finite(state.diffuse)) {
        return false;
    }
    if ((state.valid_fields & state_divergence) != 0U && !finite(state.divergence)) {
        return false;
    }
    if ((state.valid_fields & state_divergence_range) != 0U &&
        (!std::isfinite(state.divergence_azimuth_range) || state.divergence_azimuth_range < 0 ||
         !std::isfinite(state.divergence_position_range) || state.divergence_position_range < 0)) {
        return false;
    }
    return (state.valid_fields & state_channel_lock_max_distance) == 0U || !state.channel_lock_max_distance ||
           (std::isfinite(*state.channel_lock_max_distance) && *state.channel_lock_max_distance >= 0);
}

struct StagedControl {
    ObjectState target;
    bool initialized{false};
    bool seen_initial{false};
    bool seen_pcm{false};
};

constexpr std::array<std::string_view, 10> k_bed_labels{
    "M+030", "M-030", "M+000", "LFE1", "M+090", "M-090", "M+135", "M-135", "U+090", "U-090"};
constexpr std::array<std::string_view, 10> k_room_labels{
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};

class LiveTripleBalanceRenderer final : public ILiveSceneRenderer {
  public:
    LiveTripleBalanceRenderer(RendererConfig config, uint32_t layout, DiagnosticSink diagnostics)
        : config_(std::move(config)), layout_(layout), diagnostics_(std::move(diagnostics)) {}

    [[nodiscard]] Result<void> configure_generation(uint64_t generation,
                                                    std::span<const ElementDescriptor> descriptors) override {
        if (descriptors.size() > std::numeric_limits<uint32_t>::max()) {
            return make_error(ErrorCode::invalid_argument, "Live Triple Balance has too many elements");
        }
        std::vector<RuntimeElement> next;
        std::unordered_map<uint64_t, std::size_t> indices;
        std::vector<MradmTbLiveElement> numeric;
        std::array<bool, 10> bed{};
        std::string bed_object;
        for (const auto& descriptor : descriptors) {
            if (!indices.emplace(descriptor.element_id, next.size()).second ||
                (descriptor.role != ElementRole::object && descriptor.role != ElementRole::direct_speaker &&
                 descriptor.role != ElementRole::lfe)) {
                return make_error(ErrorCode::invalid_argument, "Live Triple Balance invalid or duplicate element");
            }
            MradmTbLiveElement lane{};
            if (descriptor.role != ElementRole::object) {
                const auto label = descriptor.role == ElementRole::lfe && descriptor.speaker_label.empty()
                                       ? std::string{"LFE1"}
                                       : render_common::canonical_direct_speaker_label(descriptor.speaker_label);
                // Iterator is a pointer on libc++ and a checked wrapper on MSVC.
                const auto found = // NOLINT(readability-qualified-auto)
                    std::ranges::find(k_bed_labels, label);
                if (found == k_bed_labels.end() || (descriptor.role == ElementRole::lfe && label != "LFE1")) {
                    return unsupported(descriptor.element_id, "speaker_label", "requires standard 7.1.2 bed labels");
                }
                const auto index = static_cast<std::size_t>(std::distance(k_bed_labels.begin(), found));
                if (bed.at(index)) {
                    return unsupported(descriptor.element_id, "speaker_label", "duplicate bed channel");
                }
                bed.at(index) = true;
                if (descriptor.semantic_identity && !descriptor.semantic_identity->object_id.empty()) {
                    const auto& id = descriptor.semantic_identity->object_id;
                    if (!bed_object.empty() && bed_object != id) {
                        return unsupported(descriptor.element_id, "object_id", "multiple beds are unsupported");
                    }
                    bed_object = id;
                }
                SceneDirectSpeakersBlock block;
                block.speaker_labels.emplace_back(k_room_labels.at(index));
                auto gains = triple_balance::bed_gains(block, config_.output_layout, config_.lfe_routing_mode);
                if (!gains) {
                    return tl::unexpected{gains.error()};
                }
                lane.kind = 1U;
                std::ranges::copy(*gains, std::begin(lane.gains));
            }
            // Initial states and semantic policy can override descriptor defaults. Validate the
            // effective state in render(), after the worker has applied those overrides.
            next.push_back({descriptor, default_state(descriptor), false});
            numeric.push_back(lane);
        }
        if (std::ranges::any_of(bed, [](bool present) { return present; }) &&
            (config_.sample_rate != 48000U || !std::ranges::all_of(bed, [](bool present) { return present; }))) {
            return make_error(ErrorCode::unsupported, "Live Triple Balance requires one complete 48 kHz 7.1.2 bed");
        }
        auto mixer = dsp::LiveTripleBalanceMixer::create(layout_, config_.sample_rate, numeric);
        if (!mixer) {
            return tl::unexpected{mixer.error()};
        }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        // Generation ids may be reused in another epoch. Defer the snapshot until the
        // first render supplies the epoch and the current/outgoing/incoming role.
        trace_lane_kinds_.clear();
        trace_lane_gains_.clear();
        for (const auto& lane : numeric) {
            trace_lane_kinds_.push_back(static_cast<int>(lane.kind));
            trace_lane_gains_.insert(trace_lane_gains_.end(), std::begin(lane.gains), std::end(lane.gains));
        }
        trace_lanes_pending_ = true;
#endif
        mixer_.emplace(std::move(*mixer));
        elements_ = std::move(next);
        indices_ = std::move(indices);
        generation_ = generation;
        warned_.clear();
        return {};
    }

    void reset() override {
        mixer_.reset();
        elements_.clear();
        indices_.clear();
        warned_.clear();
        generation_ = 0;
    }

    // Stage and validate the entire slice before changing numerical or semantic history.
    // NOLINTNEXTLINE(readability-function-size)
    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        const auto channels = output_channels();
        if (!mixer_ || frame.generation_id != generation_ ||
            frame.duration_samples > std::numeric_limits<std::size_t>::max() / channels ||
            output.size() < static_cast<std::size_t>(frame.duration_samples) * channels) {
            return make_error(ErrorCode::invalid_argument, "Live Triple Balance invalid generation or output buffer");
        }
        auto& mixer = mixer_.value();
        staged_.resize(elements_.size());
        planes_.assign(elements_.size(), {});
        initial_.clear();
        events_.clear();
        pending_warned_ = warned_;
        for (std::size_t i = 0; i < elements_.size(); ++i) {
            staged_[i] = {elements_[i].target_state, elements_[i].initialized, false, false};
        }
        for (const auto& plane : frame.pcm) {
            const auto found = indices_.find(plane.element_id);
            if (found == indices_.end() || staged_[found->second].seen_pcm ||
                (plane.has_signal && plane.samples.size() < frame.duration_samples)) {
                return make_error(ErrorCode::invalid_argument, "Live Triple Balance invalid PCM plane");
            }
            staged_[found->second].seen_pcm = true;
            if (plane.has_signal) {
                planes_[found->second] = {plane.samples.data(), plane.samples.size(), 1U, 0U};
            }
        }
        for (const auto& entry : frame.initial_states) {
            const auto found = indices_.find(entry.element_id);
            if (found == indices_.end() || staged_[found->second].seen_initial || !valid_state(entry.state)) {
                return make_error(ErrorCode::invalid_argument, "Live Triple Balance invalid initial state");
            }
            auto& staged = staged_[found->second];
            staged.seen_initial = true;
            staged.target = default_state(elements_[found->second].descriptor);
            copy_state_fields(staged.target, entry.state, entry.state.valid_fields);
            staged.initialized = false;
        }
        for (std::size_t i = 0; i < elements_.size(); ++i) {
            if (!staged_[i].initialized) {
                auto valid = validate_state(elements_[i].descriptor, staged_[i].target);
                if (!valid) {
                    return valid;
                }
                initial_.push_back(command(i, 0, 0, 7U));
                staged_[i].initialized = true;
            }
        }
        uint32_t previous = 0;
        for (const auto& update : frame.updates) {
            const auto found = indices_.find(update.element_id);
            const auto changed = update.changed_fields | update.cleared_fields;
            if (found == indices_.end() || update.offset_samples >= frame.duration_samples ||
                update.offset_samples < previous || (changed & ~k_known_state_fields) != 0U ||
                (update.changed_fields & ~update.state.valid_fields) != 0U || !valid_state(update.state)) {
                return make_error(ErrorCode::invalid_argument, "Live Triple Balance invalid metadata update");
            }
            previous = update.offset_samples;
            auto& target = staged_[found->second].target;
            copy_state_fields(target, default_state(elements_[found->second].descriptor), update.cleared_fields);
            target.valid_fields &= ~update.cleared_fields;
            copy_state_fields(target, update.state, update.changed_fields);
            auto valid = validate_state(elements_[found->second].descriptor, target);
            if (!valid) {
                return valid;
            }
            uint32_t duration = config_.object_smoothing_frames;
            if (update.jump_position) {
                duration = 0;
            } else if (update.ramp_duration_samples != 0) {
                duration = update.ramp_duration_samples;
            }
            uint32_t fields = 0;
            if ((changed & state_position) != 0U) {
                fields |= 1U;
            }
            if ((changed & state_extent) != 0U) {
                fields |= 2U;
            }
            if ((changed & (state_active | state_linear_gain)) != 0U) {
                fields |= 4U;
            }
            if (fields != 0U) {
                events_.push_back(command(found->second, update.offset_samples, duration, fields));
            }
        }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        trace_commands(frame);
#endif
        auto rendered = mixer.process(frame.duration_samples,
                                      planes_,
                                      initial_,
                                      events_,
                                      output.first(static_cast<std::size_t>(frame.duration_samples) * channels));
        if (!rendered || frame.duration_samples == 0U) {
            return rendered;
        }
        for (std::size_t i = 0; i < elements_.size(); ++i) {
            elements_[i].target_state = staged_[i].target;
            elements_[i].initialized = staged_[i].initialized;
        }
        if (diagnostics_) {
            for (const auto id : pending_warned_) {
                if (!warned_.contains(id)) {
                    diagnostics_({LogLevel::warning,
                                  DiagnosticCode::semantic_degraded,
                                  frame.epoch_id,
                                  frame.generation_id,
                                  id,
                                  state_extent | state_diffuse,
                                  "Triple Balance spread=none explicitly ignores extent/diffuse"});
                }
            }
        }
        warned_.swap(pending_warned_);
        return {};
    }

    [[nodiscard]] uint32_t output_channels() const noexcept override {
        return static_cast<uint32_t>(dsp::tb_channels(layout_));
    }
    [[nodiscard]] uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    [[nodiscard]] uint32_t tail_input_frames() const noexcept override { return 0U; }

  private:
    static Result<void> unsupported(uint64_t id, const std::string& field, const std::string& reason) {
        return make_error(ErrorCode::unsupported,
                          "Live Triple Balance " + reason,
                          "element=" + std::to_string(id) + "; field=" + field);
    }
    Result<void> validate_state(const ElementDescriptor& descriptor, const ObjectState& state) {
        if (!valid_state(state)) {
            return make_error(ErrorCode::invalid_argument, "Live Triple Balance invalid state");
        }
        if (state.divergence != 0 || state.channel_lock || state.screen_reference || state.head_locked) {
            return unsupported(descriptor.element_id,
                               "divergence/channelLock/screenRef/headLocked",
                               "does not support these object modifiers");
        }
        const bool extent = state.width != 0 || state.height != 0 || state.depth != 0;
        if (descriptor.role != ElementRole::object) {
            if ((state.valid_fields & state_position) != 0U || extent || state.diffuse != 0) {
                return unsupported(descriptor.element_id, "position/extent/diffuse", "bed routing is fixed");
            }
            return {};
        }
        if (std::abs(state.x) > 1 || std::abs(state.y) > 1 || state.z > 1 || state.z < (layout_ == 2U ? -1.0F : 0.0F)) {
            return unsupported(descriptor.element_id, "position", "position is outside the supported room");
        }
        if (config_.speaker_spread_mode == SpeakerSpreadMode::none) {
            if (extent || state.diffuse != 0) {
                pending_warned_.insert(descriptor.element_id);
            }
        } else if ((extent && (config_.sample_rate != 48000U || state.width < 0 || state.width > 1 ||
                               state.width != state.height || state.width != state.depth)) ||
                   (state.diffuse != 0 && (state.diffuse != 1 || !extent))) {
            return unsupported(descriptor.element_id,
                               "extent/diffuse",
                               "requires a 48 kHz equal-size Object; diffuse must be 0 or 1 with nonzero size");
        }
        return {};
    }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    // Commands as handed to the Rust mixer: position, size and level are derived in C++.
    void trace_commands(const Frame& frame) {
        const auto key = consistency::scene_renderer_key(frame) + ".30-triple";
        if (trace_lanes_pending_ && !trace_lane_kinds_.empty()) {
            consistency::dump(key + "-lane-kinds.i32", trace_lane_kinds_);
            consistency::dump(key + "-lane-gains.f32", trace_lane_gains_);
        }
        trace_lanes_pending_ = false;
        std::vector<int> commands;
        std::vector<float> values;
        for (const auto* list : {&initial_, &events_}) {
            for (const auto& command : *list) {
                commands.insert(commands.end(),
                                {static_cast<int>(command.element),
                                 static_cast<int>(command.offset),
                                 static_cast<int>(command.duration),
                                 static_cast<int>(command.fields)});
                values.insert(
                    values.end(),
                    {command.position.x, command.position.y, command.position.z, command.size, command.level});
            }
        }
        if (!commands.empty()) {
            consistency::dump(key + "-commands.i32", commands);
            consistency::dump(key + "-values.f32", values);
        }
    }
#endif
    [[nodiscard]] MradmTbLiveCommand
    command(std::size_t index, uint32_t offset, uint32_t duration, uint32_t fields) const {
        const auto& state = staged_[index].target;
        const bool object = elements_[index].descriptor.role == ElementRole::object;
        return {static_cast<uint32_t>(index),
                offset,
                duration,
                fields,
                object ? MradmTbPosition{state.x, state.y, state.z} : MradmTbPosition{0, 1, 0},
                config_.speaker_spread_mode == SpeakerSpreadMode::none ? 0 : state.width,
                level_for(state)};
    }

    RendererConfig config_;
    uint32_t layout_;
    DiagnosticSink diagnostics_;
    uint64_t generation_{};
    std::optional<dsp::LiveTripleBalanceMixer> mixer_;
    std::vector<RuntimeElement> elements_;
    std::unordered_map<uint64_t, std::size_t> indices_;
    std::vector<StagedControl> staged_;
    std::vector<MradmTbLivePlane> planes_;
    std::vector<MradmTbLiveCommand> initial_, events_;
    std::unordered_set<uint64_t> warned_, pending_warned_;
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    std::vector<int> trace_lane_kinds_;
    std::vector<float> trace_lane_gains_;
    bool trace_lanes_pending_{false};
#endif
};
} // namespace

Result<std::unique_ptr<ILiveSceneRenderer>> create_live_triple_balance_renderer(const RendererConfig& config,
                                                                                DiagnosticSink diagnostics) {
    auto layout = dsp::tb_layout(config.output_layout);
    if (!layout) {
        return tl::unexpected{layout.error()};
    }
    if (config.speaker_geometry != SpeakerGeometry::standard || config.speaker_spread_mode == SpeakerSpreadMode::mdap ||
        !config.sofa_path.empty() || (*layout == 2U && config.sample_rate != 48000U)) {
        return make_error(ErrorCode::unsupported,
                          "Live Triple Balance requires fixed room geometry, no MDAP/SOFA and 48 kHz for 22.2");
    }
    return std::unique_ptr<ILiveSceneRenderer>{
        std::make_unique<LiveTripleBalanceRenderer>(config, *layout, std::move(diagnostics))};
}
} // namespace mradm::live_scene
