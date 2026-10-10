#include "live_vbap_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/scene.h"

#include "dsp.h"
#include "live_scene_trace.h"
#include "live_vbap.h"
#include "render_common.h"
#include "scene_math.h"
#include "speaker_layouts.h"

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

[[nodiscard]] SceneBlockPosition canonical_position(float x, float y, float z) {
    SceneBlockPosition position;
    position.cartesian = true;
    position.x = x;
    position.y = y;
    position.z = z;
    return position;
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

class LiveVbapRenderer final : public ILiveSceneRenderer {
  public:
    LiveVbapRenderer(RendererConfig config, render_layouts::SpeakerLayout layout, DiagnosticSink diagnostics)
        : config_(std::move(config)), layout_(std::move(layout)), diagnostics_(std::move(diagnostics)) {
        routing_targets_.reserve(layout_.speakers.size());
        std::ranges::transform(layout_.speakers, std::back_inserter(routing_targets_), [](const auto& speaker) {
            return render_common::DirectSpeakerRoutingTarget{
                speaker.label, speaker.azimuth, speaker.elevation, speaker.is_lfe};
        });
    }

    [[nodiscard]] Result<void> configure_generation(std::uint64_t generation_id,
                                                    std::span<const ElementDescriptor> elements) override {
        if (elements.size() > std::numeric_limits<std::uint32_t>::max()) {
            return make_error(ErrorCode::invalid_argument, "Live VBAP element count overflows numerical indices");
        }
        std::vector<RuntimeElement> next;
        std::unordered_map<std::uint64_t, std::size_t> indices;
        next.reserve(elements.size());
        indices.reserve(elements.size());
        for (const auto& descriptor : elements) {
            if ((descriptor.role != ElementRole::object && descriptor.role != ElementRole::direct_speaker &&
                 descriptor.role != ElementRole::lfe) ||
                !indices.emplace(descriptor.element_id, next.size()).second) {
                return make_error(ErrorCode::invalid_argument, "Live VBAP has an invalid or duplicate element");
            }
            auto defaults = default_state(descriptor);
            if (descriptor.has_position) {
                auto position = defaults;
                position.valid_fields |= state_position;
                if (!valid_state(position)) {
                    return make_error(ErrorCode::invalid_argument, "Live VBAP element position is invalid");
                }
            }
            next.push_back({descriptor, defaults, false});
        }
        auto mixer = dsp::LiveVbapMixer::create(static_cast<std::uint32_t>(next.size()), output_channels());
        if (!mixer) {
            return tl::unexpected{mixer.error()};
        }
        mixer_.emplace(std::move(*mixer));
        elements_.swap(next);
        element_index_.swap(indices);
        generation_id_ = generation_id;
        warned_fields_.clear();
        return {};
    }

    void reset() override {
        if (mixer_) {
            mixer_->reset();
        }
        mixer_.reset();
        generation_id_ = 0;
        elements_.clear();
        element_index_.clear();
        warned_fields_.clear();
        staged_.clear();
        pending_diagnostics_.clear();
        pending_warned_fields_.clear();
        planes_.clear();
        initial_.clear();
        events_.clear();
        coefficients_.clear();
    }

    // Prepare the whole frame before committing either numeric or semantic state.
    // NOLINTNEXTLINE(readability-function-size)
    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        const auto channels = layout_.speakers.size();
        if (frame.duration_samples > std::numeric_limits<std::size_t>::max() / channels ||
            output.size() < static_cast<std::size_t>(frame.duration_samples) * channels) {
            return make_error(ErrorCode::invalid_argument, "live VBAP output buffer is too small");
        }
        if (!mixer_ || frame.generation_id != generation_id_) {
            return make_error(ErrorCode::invalid_argument, "live VBAP frame uses an unconfigured generation");
        }
        auto& mixer = mixer_.value();
        const auto required = static_cast<std::size_t>(frame.duration_samples) * channels;
        staged_.resize(elements_.size());
        planes_.assign(elements_.size(), MradmLiveVbapPlane{});
        initial_.clear();
        events_.clear();
        coefficients_.clear();
        pending_diagnostics_.clear();
        pending_warned_fields_ = warned_fields_;
        for (std::size_t i = 0; i < elements_.size(); ++i) {
            staged_[i] = {elements_[i].target_state, elements_[i].initialized, false, false};
        }
        for (const auto& plane : frame.pcm) {
            const auto found = element_index_.find(plane.element_id);
            if (found == element_index_.end() || staged_[found->second].seen_pcm ||
                (plane.has_signal && plane.samples.size() < frame.duration_samples)) {
                return make_error(ErrorCode::invalid_argument, "Live VBAP PCM plane has an invalid ID or length");
            }
            staged_[found->second].seen_pcm = true;
            if (plane.has_signal) {
                planes_[found->second] = {plane.samples.data(), plane.samples.size(), 1U, 0U};
            }
        }
        for (const auto& state : frame.initial_states) {
            const auto found = element_index_.find(state.element_id);
            if (found == element_index_.end() || staged_[found->second].seen_initial || !valid_state(state.state)) {
                return make_error(ErrorCode::invalid_argument, "Live VBAP initial state is invalid");
            }
            auto& staged = staged_[found->second];
            staged.seen_initial = true;
            staged.target = default_state(elements_[found->second].descriptor);
            copy_state_fields(staged.target, state.state, state.state.valid_fields);
            auto prepared = prepare_initial(found->second, frame);
            if (!prepared) {
                return prepared;
            }
        }
        for (std::size_t i = 0; i < staged_.size(); ++i) {
            if (!staged_[i].initialized) {
                auto prepared = prepare_initial(i, frame);
                if (!prepared) {
                    return prepared;
                }
            }
        }
        std::uint32_t previous = 0;
        for (const auto& update : frame.updates) {
            const auto found = element_index_.find(update.element_id);
            const auto changed = update.changed_fields | update.cleared_fields;
            if (found == element_index_.end() || update.offset_samples >= frame.duration_samples ||
                update.offset_samples < previous || (changed & ~k_known_state_fields) != 0U ||
                (update.changed_fields & ~update.state.valid_fields) != 0U || !valid_state(update.state)) {
                return make_error(ErrorCode::invalid_argument, "Live VBAP metadata update is invalid");
            }
            previous = update.offset_samples;
            auto& target = staged_[found->second].target;
            target.valid_fields &= ~update.cleared_fields;
            copy_state_fields(target, update.state, update.changed_fields);
            std::uint32_t ramp = config_.object_smoothing_frames;
            if (update.jump_position) {
                ramp = 0;
            } else if (update.ramp_duration_samples != 0) {
                ramp = update.ramp_duration_samples;
            }
            MradmLiveVbapCommand command{};
            command.element = static_cast<std::uint32_t>(found->second);
            command.offset = update.offset_samples;
            command.duration = ramp;
            if ((changed & (state_active | state_linear_gain)) != 0U) {
                command.fields |= 2U;
                command.level = level_for(target);
            }
            if ((changed & ~(state_active | state_linear_gain | state_head_locked)) != 0U) {
                auto gains = gains_for(elements_[found->second], target, frame);
                if (!gains) {
                    return tl::unexpected{gains.error()};
                }
                command.fields |= 1U;
                command.coefficient_offset = coefficients_.size();
                coefficients_.insert(coefficients_.end(), gains->begin(), gains->end());
            }
            if (command.fields != 0U) {
                events_.push_back(command);
            }
        }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        trace_commands(frame);
#endif
        auto rendered =
            mixer.process(frame.duration_samples, planes_, initial_, events_, coefficients_, output.first(required));
        if (!rendered) {
            return rendered;
        }
        for (std::size_t i = 0; i < elements_.size(); ++i) {
            elements_[i].target_state = staged_[i].target;
            elements_[i].initialized = staged_[i].initialized;
        }
        warned_fields_.swap(pending_warned_fields_);
        for (auto& diagnostic : pending_diagnostics_) {
            diagnostics_(std::move(diagnostic));
        }
        return {};
    }

    [[nodiscard]] std::uint32_t output_channels() const noexcept override {
        return static_cast<std::uint32_t>(layout_.speakers.size());
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override { return 0U; }

  private:
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    // Commands and panning coefficients exactly as handed to the Rust mixer.
    void trace_commands(const Frame& frame) const {
        std::vector<int> commands;
        std::vector<float> levels;
        for (const auto* list : {&initial_, &events_}) {
            for (const auto& command : *list) {
                commands.insert(commands.end(),
                                {static_cast<int>(command.element),
                                 static_cast<int>(command.offset),
                                 static_cast<int>(command.duration),
                                 static_cast<int>(command.fields),
                                 static_cast<int>(command.coefficient_offset)});
                levels.push_back(command.level);
            }
        }
        const auto key = consistency::scene_renderer_key(frame) + ".30-vbap" + std::to_string(layout_.speakers.size());
        if (!commands.empty()) {
            consistency::dump(key + "-commands.i32", commands);
            consistency::dump(key + "-levels.f32", levels);
        }
        if (!coefficients_.empty()) {
            consistency::dump(key + "-coefficients.f32", coefficients_);
        }
    }
#endif
    Result<void> prepare_initial(std::size_t index, const Frame& frame) {
        auto gains = gains_for(elements_[index], staged_[index].target, frame);
        if (!gains) {
            return tl::unexpected{gains.error()};
        }
        initial_.push_back({static_cast<std::uint32_t>(index),
                            0U,
                            0U,
                            3U,
                            coefficients_.size(),
                            level_for(staged_[index].target),
                            0U});
        coefficients_.insert(coefficients_.end(), gains->begin(), gains->end());
        staged_[index].initialized = true;
        return {};
    }
    [[nodiscard]] bool is_2d() const {
        return std::ranges::all_of(layout_.speakers, [](const auto& speaker) {
            return speaker.is_lfe || std::fabs(speaker.elevation) < 1.0e-6F;
        });
    }

    [[nodiscard]] Result<std::vector<float>>
    point_gains(float azimuth, float elevation, float gain, float spread_deg) const {
        std::vector<float> speakers;
        speakers.reserve(layout_.speakers.size() * 2U);
        for (const auto& speaker : layout_.speakers) {
            if (!speaker.is_lfe) {
                speakers.push_back(speaker.azimuth);
                speakers.push_back(speaker.elevation);
            }
        }
        if (speakers.size() < 4U) {
            return make_error(ErrorCode::unsupported, "live VBAP layout has fewer than two non-LFE speakers");
        }

        std::vector<float> table;
        try {
            table = dsp::panner_for(speakers, !is_2d())->gains(azimuth, elevation, spread_deg);
        } catch (const std::exception& error) {
            return make_error(ErrorCode::render_failed, error.what());
        }

        std::vector<float> gains(layout_.speakers.size(), 0.0F);
        std::size_t source_index = 0;
        for (std::size_t channel = 0; channel < layout_.speakers.size(); ++channel) {
            if (!layout_.speakers[channel].is_lfe) {
                gains[channel] = table[source_index++] * gain;
            }
        }
        return gains;
    }

    [[nodiscard]] Result<std::vector<float>>
    gains_for(const RuntimeElement& element, const ObjectState& state, const Frame& frame) {
        // Unit-level panning avoids coupling a new gain target to an older
        // position ramp. The independent content level is applied per sample.
        constexpr float linear_gain = 1.0F;
        if (element.descriptor.role == ElementRole::lfe) {
            return lfe_gains(element.descriptor, linear_gain, frame);
        }
        if (element.descriptor.role == ElementRole::direct_speaker) {
            return direct_speaker_gains(element.descriptor, state, linear_gain, frame);
        }

        SceneObjectBlock block;
        block.position = canonical_position(state.x, state.y, state.z);
        block.gain = linear_gain;
        block.width = state.width;
        block.height = state.height;
        block.depth = state.depth;
        block.diffuse = state.diffuse;
        block.divergence = state.divergence;
        block.divergence_azimuth_range = state.divergence_azimuth_range;
        block.divergence_position_range = state.divergence_position_range;
        block.channel_lock = state.channel_lock;
        block.channel_lock_max_distance = state.channel_lock_max_distance;
        block.screen_ref = state.screen_reference;
        block.head_locked = state.head_locked;
        if (block.channel_lock) {
            std::vector<SceneOutputSpeaker> speakers;
            speakers.reserve(layout_.speakers.size());
            std::ranges::transform(layout_.speakers, std::back_inserter(speakers), [](const auto& speaker) {
                return SceneOutputSpeaker{speaker.azimuth, speaker.elevation, speaker.is_lfe};
            });
            block = apply_channel_lock(block, speakers);
        }

        std::vector<float> result(layout_.speakers.size(), 0.0F);
        for (const auto& source : expand_object_divergence(block)) {
            const auto position = scene_position_to_polar(source.position);
            const float distance = std::max(0.4F, position.distance);
            float spread = 0.0F;
            if (!is_2d() && config_.speaker_spread_mode != SpeakerSpreadMode::none) {
                spread =
                    dsp::scene_math<6, 1>(7U, {source.width, source.height, source.depth, distance, 0.0F, 0.0F})[0];
            }
            auto source_gains = point_gains(position.azimuth, position.elevation, source.gain, spread);
            if (!source_gains) {
                return tl::unexpected{source_gains.error()};
            }
            for (std::size_t channel = 0; channel < result.size(); ++channel) {
                result[channel] += (*source_gains)[channel];
            }
        }

        if ((state.valid_fields & state_diffuse) != 0U && state.diffuse > 1.0e-4F) {
            warn_once(frame, element.descriptor.element_id, state_diffuse, "VBAP treats diffuse as direct energy");
        }
        if ((state.valid_fields & state_screen_reference) != 0U && state.screen_reference) {
            warn_once(frame,
                      element.descriptor.element_id,
                      state_screen_reference,
                      "VBAP has no live screen reference transform; canonical position is used");
        }
        if ((state.valid_fields & state_head_locked) != 0U && state.head_locked) {
            warn_once(frame,
                      element.descriptor.element_id,
                      state_head_locked,
                      "VBAP has no listener-orientation stage; head-locked is currently neutral");
        }
        return result;
    }

    [[nodiscard]] Result<std::vector<float>> direct_speaker_gains(const ElementDescriptor& descriptor,
                                                                  const ObjectState& state,
                                                                  float gain,
                                                                  const Frame& frame) {
        std::vector<std::string> labels;
        if (!descriptor.speaker_label.empty()) {
            labels.push_back(descriptor.speaker_label);
        }

        SceneBlockPosition position;
        bool has_position = (state.valid_fields & state_position) != 0U;
        if (has_position) {
            position = canonical_position(state.x, state.y, state.z);
        } else if (const auto target = render_common::direct_speaker_index_for_labels(routing_targets_, labels);
                   target && !layout_.speakers[*target].is_lfe) {
            std::vector<float> gains(layout_.speakers.size(), 0.0F);
            gains[*target] = gain;
            return gains;
        } else if (descriptor.has_position) {
            position = canonical_position(descriptor.x, descriptor.y, descriptor.z);
            has_position = true;
        } else if (const auto label_position = render_common::direct_speaker_position_for_labels(labels)) {
            position.azimuth = label_position->azimuth;
            position.elevation = label_position->elevation;
            position.distance = 1.0F;
            has_position = true;
        }
        if (!has_position) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("DirectSpeakers element {} has neither a routable label nor position",
                                          descriptor.element_id));
        }
        const auto polar = scene_position_to_polar(position);
        warn_once(frame,
                  descriptor.element_id,
                  0,
                  "DirectSpeakers uses its current canonical position instead of fixed label routing",
                  DiagnosticCode::direct_speaker_fallback);
        return point_gains(polar.azimuth, polar.elevation, gain, 0.0F);
    }

    [[nodiscard]] Result<std::vector<float>>
    lfe_gains(const ElementDescriptor& descriptor, float gain, const Frame& frame) {
        std::vector<std::size_t> lfe_channels;
        for (std::size_t channel = 0; channel < layout_.speakers.size(); ++channel) {
            if (layout_.speakers[channel].is_lfe) {
                lfe_channels.push_back(channel);
            }
        }
        if (lfe_channels.empty()) {
            warn_once(frame,
                      descriptor.element_id,
                      0,
                      "LFE element dropped because the output layout has no LFE channel",
                      DiagnosticCode::missing_lfe_output);
            return std::vector<float>(layout_.speakers.size(), 0.0F);
        }
        std::vector<float> gains(layout_.speakers.size(), 0.0F);
        if (config_.lfe_routing_mode == LfeRoutingMode::split_power && lfe_channels.size() >= 2U) {
            gains[lfe_channels[0]] = gain * render_common::k_lfe_split_power_gain;
            gains[lfe_channels[1]] = gain * render_common::k_lfe_split_power_gain;
            return gains;
        }
        if (!descriptor.speaker_label.empty()) {
            const std::string wanted = render_common::canonical_direct_speaker_label(descriptor.speaker_label);
            const auto target = std::ranges::find_if(lfe_channels, [&](const auto channel) {
                return render_common::canonical_direct_speaker_label(layout_.speakers[channel].label) == wanted;
            });
            if (target != lfe_channels.end()) {
                gains[*target] = gain;
                return gains;
            }
        }
        gains[lfe_channels.front()] = gain;
        return gains;
    }

    void warn_once(const Frame& frame,
                   std::uint64_t element_id,
                   std::uint64_t field,
                   std::string message,
                   DiagnosticCode code = DiagnosticCode::semantic_degraded) {
        const std::uint64_t key = (field << 8U) ^ static_cast<std::uint64_t>(code);
        if (!pending_warned_fields_.insert(key).second || !diagnostics_) {
            return;
        }
        pending_diagnostics_.push_back(
            {LogLevel::warning, code, frame.epoch_id, frame.generation_id, element_id, field, std::move(message)});
    }

    RendererConfig config_;
    render_layouts::SpeakerLayout layout_;
    DiagnosticSink diagnostics_;
    std::vector<render_common::DirectSpeakerRoutingTarget> routing_targets_;
    std::uint64_t generation_id_{0};
    std::vector<RuntimeElement> elements_;
    std::unordered_map<std::uint64_t, std::size_t> element_index_;
    std::unordered_set<std::uint64_t> warned_fields_;
    std::optional<dsp::LiveVbapMixer> mixer_;
    std::vector<StagedControl> staged_;
    std::vector<MradmLiveVbapPlane> planes_;
    std::vector<MradmLiveVbapCommand> initial_;
    std::vector<MradmLiveVbapCommand> events_;
    std::vector<float> coefficients_;
    std::unordered_set<std::uint64_t> pending_warned_fields_;
    std::vector<Diagnostic> pending_diagnostics_;
};

} // namespace

Result<std::unique_ptr<ILiveSceneRenderer>> create_live_vbap_renderer(const RendererConfig& config,
                                                                      DiagnosticSink diagnostics) {
    const auto* layout = render_layouts::find_speaker_layout(config.output_layout, config.speaker_geometry);
    if (layout == nullptr) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("unsupported live VBAP output layout '{}'", config.output_layout));
    }
    if (layout->speakers.empty()) {
        return make_error(ErrorCode::unsupported, "live VBAP output layout has no speakers");
    }
    return std::unique_ptr<ILiveSceneRenderer>{
        std::make_unique<LiveVbapRenderer>(config, *layout, std::move(diagnostics))};
}

} // namespace mradm::live_scene
