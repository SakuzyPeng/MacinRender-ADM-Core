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
#include <saf_vbap.h>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/scene.h"

#include "render_common.h"
#include "speaker_layouts.h"

namespace mradm::live_scene {

namespace {

struct SafFree {
    void operator()(float* ptr) const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        std::free(ptr);
    }
};

struct RuntimeElement {
    ElementDescriptor descriptor;
    ObjectState target_state;
    std::vector<float> current_gains;
    std::vector<float> target_gains;
    std::vector<float> gain_steps;
    std::uint32_t ramp_remaining{0};
    bool initialized{false};
};

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
        generation_id_ = generation_id;
        elements_.clear();
        element_index_.clear();
        warned_fields_.clear();
        elements_.reserve(elements.size());
        for (const auto& descriptor : elements) {
            RuntimeElement runtime;
            runtime.descriptor = descriptor;
            runtime.target_state = default_state(descriptor);
            runtime.current_gains.assign(layout_.speakers.size(), 0.0F);
            runtime.target_gains.assign(layout_.speakers.size(), 0.0F);
            runtime.gain_steps.assign(layout_.speakers.size(), 0.0F);
            element_index_.emplace(descriptor.element_id, elements_.size());
            elements_.push_back(std::move(runtime));
        }
        return {};
    }

    void reset() override {
        generation_id_ = 0;
        elements_.clear();
        element_index_.clear();
        warned_fields_.clear();
    }

    // Rendering is kept as one sample loop so metadata retargets and gain ramps share an exact ordering point.
    // NOLINTNEXTLINE(readability-function-size)
    [[nodiscard]] Result<void> render(const Frame& frame, std::span<float> output) override {
        const std::size_t required = static_cast<std::size_t>(frame.duration_samples) * layout_.speakers.size();
        if (output.size() < required) {
            return make_error(ErrorCode::invalid_argument, "live VBAP output buffer is too small");
        }
        if (frame.generation_id != generation_id_) {
            return make_error(ErrorCode::invalid_argument, "live VBAP frame uses an unconfigured generation");
        }
        std::ranges::fill(output.first(required), 0.0F);

        for (const auto& initial : frame.initial_states) {
            const auto found = element_index_.find(initial.element_id);
            if (found == element_index_.end()) {
                return make_error(ErrorCode::invalid_argument, "initial state references an unknown element");
            }
            auto& element = elements_[found->second];
            ObjectState state = default_state(element.descriptor);
            copy_state_fields(state, initial.state, initial.state.valid_fields);
            element.target_state = state;
            auto gains = gains_for(element, state, frame);
            if (!gains) {
                return tl::unexpected{gains.error()};
            }
            element.current_gains = *gains;
            element.target_gains = std::move(*gains);
            std::ranges::fill(element.gain_steps, 0.0F);
            element.ramp_remaining = 0;
            element.initialized = true;
        }

        for (auto& element : elements_) {
            if (!element.initialized) {
                const ObjectState defaults = default_state(element.descriptor);
                element.target_state = defaults;
                auto gains = gains_for(element, defaults, frame);
                if (!gains) {
                    return tl::unexpected{gains.error()};
                }
                element.current_gains = *gains;
                element.target_gains = std::move(*gains);
                element.initialized = true;
            }
        }

        std::unordered_map<std::uint64_t, const PcmPlane*> pcm;
        pcm.reserve(frame.pcm.size());
        for (const auto& plane : frame.pcm) {
            pcm.emplace(plane.element_id, &plane);
        }

        std::size_t update_index = 0;
        for (std::uint32_t sample = 0; sample < frame.duration_samples; ++sample) {
            while (update_index < frame.updates.size() && frame.updates[update_index].offset_samples == sample) {
                const auto& update = frame.updates[update_index++];
                const auto found = element_index_.find(update.element_id);
                if (found == element_index_.end()) {
                    return make_error(ErrorCode::invalid_argument, "metadata update references an unknown element");
                }
                auto& element = elements_[found->second];
                ObjectState target = element.target_state;
                target.valid_fields &= ~update.cleared_fields;
                copy_state_fields(target, update.state, update.changed_fields);
                element.target_state = target;
                auto gains = gains_for(element, target, frame);
                if (!gains) {
                    return tl::unexpected{gains.error()};
                }
                std::uint32_t ramp = config_.object_smoothing_frames;
                if (update.jump_position) {
                    ramp = 0U;
                } else if (update.ramp_duration_samples != 0U) {
                    ramp = update.ramp_duration_samples;
                }
                set_target(element, std::move(*gains), ramp);
            }

            for (auto& element : elements_) {
                const auto plane_it = pcm.find(element.descriptor.element_id);
                if (plane_it == pcm.end() || !plane_it->second->has_signal) {
                    advance_ramp(element);
                    continue;
                }
                const float input = plane_it->second->samples[sample];
                for (std::size_t channel = 0; channel < layout_.speakers.size(); ++channel) {
                    output[(static_cast<std::size_t>(sample) * layout_.speakers.size()) + channel] +=
                        input * element.current_gains[channel];
                }
                advance_ramp(element);
            }
        }
        return {};
    }

    [[nodiscard]] std::uint32_t output_channels() const noexcept override {
        return static_cast<std::uint32_t>(layout_.speakers.size());
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return config_.sample_rate; }
    [[nodiscard]] std::uint32_t tail_input_frames() const noexcept override { return 0U; }

  private:
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

        std::vector<float> source{azimuth, elevation};
        float* raw_table = nullptr;
        int table_size = 0;
        int simplex_count = 0;
        if (is_2d()) {
            generateVBAPgainTable2D_srcs(source.data(),
                                         1,
                                         speakers.data(),
                                         static_cast<int>(speakers.size() / 2U),
                                         &raw_table,
                                         &table_size,
                                         &simplex_count);
        } else {
            generateVBAPgainTable3D_srcs(source.data(),
                                         1,
                                         speakers.data(),
                                         static_cast<int>(speakers.size() / 2U),
                                         1,
                                         1,
                                         spread_deg,
                                         &raw_table,
                                         &table_size,
                                         &simplex_count);
        }
        std::unique_ptr<float, SafFree> table{raw_table};
        if (table == nullptr || table_size != 1) {
            return make_error(ErrorCode::render_failed, "SAF live VBAP gain calculation failed");
        }

        std::vector<float> gains(layout_.speakers.size(), 0.0F);
        std::size_t source_index = 0;
        for (std::size_t channel = 0; channel < layout_.speakers.size(); ++channel) {
            if (!layout_.speakers[channel].is_lfe) {
                gains[channel] = table.get()[source_index++] * gain;
            }
        }
        return gains;
    }

    [[nodiscard]] Result<std::vector<float>>
    gains_for(const RuntimeElement& element, const ObjectState& state, const Frame& frame) {
        const float linear_gain = (state.valid_fields & state_linear_gain) != 0U ? state.linear_gain : 1.0F;
        const bool active = (state.valid_fields & state_active) == 0U || state.active;
        if (!active || linear_gain == 0.0F) {
            return std::vector<float>(layout_.speakers.size(), 0.0F);
        }
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
                const float scale = std::clamp(1.0F / distance, 0.5F, 2.5F);
                const float width = source.width * 60.0F * scale;
                const float height = source.height * 45.0F * scale;
                const float depth = source.depth * 20.0F * scale;
                spread = std::min(180.0F, render_common::canonical_vector_length(width, height, depth));
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

    void set_target(RuntimeElement& element, std::vector<float> gains, std::uint32_t ramp_samples) {
        element.target_gains = std::move(gains);
        if (ramp_samples == 0U) {
            element.current_gains = element.target_gains;
            std::ranges::fill(element.gain_steps, 0.0F);
            element.ramp_remaining = 0;
            return;
        }
        element.ramp_remaining = ramp_samples;
        for (std::size_t channel = 0; channel < element.gain_steps.size(); ++channel) {
            element.gain_steps[channel] =
                (element.target_gains[channel] - element.current_gains[channel]) / static_cast<float>(ramp_samples);
        }
    }

    static void advance_ramp(RuntimeElement& element) {
        if (element.ramp_remaining == 0U) {
            return;
        }
        for (std::size_t channel = 0; channel < element.current_gains.size(); ++channel) {
            element.current_gains[channel] += element.gain_steps[channel];
        }
        --element.ramp_remaining;
        if (element.ramp_remaining == 0U) {
            element.current_gains = element.target_gains;
        }
    }

    void warn_once(const Frame& frame,
                   std::uint64_t element_id,
                   std::uint64_t field,
                   std::string message,
                   DiagnosticCode code = DiagnosticCode::semantic_degraded) {
        const std::uint64_t key = (field << 8U) ^ static_cast<std::uint64_t>(code);
        if (!warned_fields_.insert(key).second || !diagnostics_) {
            return;
        }
        diagnostics_(
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
