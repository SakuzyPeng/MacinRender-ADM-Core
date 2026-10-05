// Frozen numerical operations from 577fe2d; test-only.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "live_vbap_ffi.h"
namespace live_vbap_numeric_legacy {
struct Element {
    std::vector<float> current_gains;
    std::vector<float> target_gains;
    std::vector<float> gain_steps;
    std::uint32_t ramp_remaining{0};
    float current_level{1.0F};
    float target_level{1.0F};
    float level_step{0.0F};
    std::uint32_t level_remaining{0};
};
void set_target(Element& element, std::vector<float> gains, std::uint32_t ramp_samples) {
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

void set_level(Element& element, float target, std::uint32_t frames) {
    element.target_level = target;
    element.level_remaining = frames;
    element.level_step = frames == 0U ? 0.0F : (target - element.current_level) / static_cast<float>(frames);
    if (frames == 0U) {
        element.current_level = target;
    }
}

void advance_ramp(Element& element) {
    if (element.level_remaining != 0U) {
        element.current_level += element.level_step;
        --element.level_remaining;
        if (element.level_remaining == 0U) {
            element.current_level = element.target_level;
        }
    }
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


struct Mixer {
    Mixer(std::size_t count, std::size_t width) : elements(count), channels(width) {
        for (auto& e : elements) {
            e.current_gains.assign(width, 0.0F);
            e.target_gains.assign(width, 0.0F);
            e.gain_steps.assign(width, 0.0F);
        }
    }
    void apply(const MradmLiveVbapCommand& c, std::span<const float> coefficients) {
        auto& element = elements[c.element];
        if ((c.fields & 2U) != 0U) {
            set_level(element, c.level, c.duration);
        }
        if ((c.fields & 1U) != 0U) {
            const auto row = coefficients.subspan(static_cast<std::size_t>(c.coefficient_offset), channels);
            set_target(element, std::vector<float>(row.begin(), row.end()), c.duration);
        }
    }
    void process(uint32_t frames,
                 std::span<const MradmLiveVbapPlane> planes,
                 std::span<const MradmLiveVbapCommand> initial,
                 std::span<const MradmLiveVbapCommand> events,
                 std::span<const float> coefficients,
                 std::span<float> output) {
        std::fill_n(output.begin(), static_cast<std::size_t>(frames) * channels, 0.0F);
        for (const auto& c : initial) {
            apply(c, coefficients);
        }
        std::size_t update_index = 0;
        for (uint32_t sample = 0; sample < frames; ++sample) {
            while (update_index < events.size() && events[update_index].offset == sample) {
                apply(events[update_index++], coefficients);
            }
            for (std::size_t i = 0; i < elements.size(); ++i) {
                auto& element = elements[i];
                if (planes[i].has_signal == 0U) {
                    advance_ramp(element);
                    continue;
                }
                const float input = planes[i].samples[sample];
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    output[(static_cast<std::size_t>(sample) * channels) + channel] +=
                        input * element.current_gains[channel] * element.current_level;
                }
                advance_ramp(element);
            }
        }
    }
    std::vector<Element> elements;
    std::size_t channels;
};
} // namespace live_vbap_numeric_legacy
