#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../reference/live_vbap/legacy.h"
#include "../reference/live_vbap/ramps.h"
#include "live_vbap.h"
#include "live_vbap_renderer.h"

namespace {
using namespace mradm;
using namespace mradm::live_scene;
void require(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}
template <class T> T take(Result<T> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
    return std::move(*value);
}
void done(Result<void> value) {
    if (!value) {
        throw std::runtime_error(value.error().message);
    }
}
struct Stats {
    uint64_t values{0};
    uint64_t differing{0};
    double max_error{0};
};
void compare(Stats& stats, float actual, float reference, bool exact = false) {
    ++stats.values;
    if (std::isnan(reference)) {
        require(std::isnan(actual), "nonfinite classification mismatch");
        return;
    }
    if (std::isinf(reference)) {
        require(actual == reference, "infinity mismatch");
        return;
    }
    require(std::isfinite(actual), "unexpected nonfinite result");
    const bool same = std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(reference);
    stats.differing += same ? 0U : 1U;
    const double error = std::abs(static_cast<double>(actual) - reference);
    stats.max_error = std::max(stats.max_error, error);
    require(exact ? same : error <= 2e-6 + (2e-6 * std::abs(static_cast<double>(reference))), "numeric mismatch");
}
MradmLiveVbapCommand
command(uint32_t element, uint32_t offset, uint32_t duration, uint32_t fields, uint64_t coefficient, float level) {
    return {element, offset, duration, fields, coefficient, level, 0};
}
// Matrix cases keep every state assertion beside its corresponding render.
// NOLINTNEXTLINE(readability-function-size)
void numeric_targets(Stats& pcm, Stats& states, uint64_t& counters) {
    for (uint32_t channels : {1U, 2U, 6U, 12U, 24U}) {
        for (uint32_t elements : {0U, 1U, 3U, 9U}) {
            live_vbap_numeric_legacy::Mixer before(elements, channels);
            auto original = take(dsp::LiveVbapMixer::create(elements, channels));
            auto after = std::move(original);
            std::vector<float> coefficients(static_cast<std::size_t>(channels) * 4);
            for (std::size_t i = 0; i < coefficients.size(); ++i) {
                coefficients[i] = static_cast<float>(static_cast<int>((i * 13) % 17) - 8) / 8.0F;
            }
            for (std::size_t iteration = 0; iteration < 52; ++iteration) {
                if (iteration == 29) {
                    before = live_vbap_numeric_legacy::Mixer(elements, channels);
                    after.reset();
                }
                const auto frames =
                    std::array<uint32_t, 13>{0, 1, 7, 31, 32, 33, 127, 511, 512, 513, 1023, 1024, 1025}.at(iteration %
                                                                                                           13);
                std::vector<float> input(frames);
                for (std::size_t f = 0; f < input.size(); ++f) {
                    input[f] = static_cast<float>(static_cast<int>((f + iteration) % 31) - 15) / 16.0F;
                }
                std::vector<MradmLiveVbapPlane> planes;
                std::vector<MradmLiveVbapCommand> initial;
                for (uint32_t e = 0; e < elements; ++e) {
                    const bool signal = ((e + iteration) % 5) != 2;
                    planes.push_back(
                        {signal ? input.data() : nullptr, signal ? input.size() : 0, signal ? 1U : 0U, 0U});
                    if (iteration % 11 == 0) {
                        initial.push_back(command(e, 0, 0, 3, static_cast<uint64_t>(e % 4) * channels, 0.75F));
                    }
                }
                std::vector<MradmLiveVbapCommand> events;
                if (elements != 0 && frames != 0) {
                    events = {command(0, 0, 37, 3, channels, 0.25F),
                              command(elements - 1, frames / 3, 2049, 1, static_cast<uint64_t>(channels) * 2U, 0.0F),
                              command(0, frames / 3, 9, 2, 0, 0.5F),
                              command(0, frames / 3, 9, 2, 0, 0.5F),
                              command(elements - 1, frames - 1, 0, 3, static_cast<uint64_t>(channels) * 3U, 1.0F)};
                }
                std::vector<float> expected(static_cast<std::size_t>(frames + 1) * channels, 17.0F);
                auto actual = expected;
                before.process(frames, planes, initial, events, coefficients, expected);
                done(after.process(frames, planes, initial, events, coefficients, actual));
                for (std::size_t i = 0; i < actual.size(); ++i) {
                    compare(pcm, actual[i], expected[i], i >= static_cast<std::size_t>(frames) * channels);
                }
                for (uint32_t e = 0; e < elements; ++e) {
                    std::vector<float> gains(static_cast<std::size_t>(channels) * 3);
                    MradmLiveVbapStatus status{};
                    require(mradm_dsp_live_vbap_snapshot(
                                after.get(), e, gains.data(), gains.size(), &status, nullptr, 0) == 0,
                            "snapshot failed");
                    const auto& reference = before.elements[e];
                    compare(states, status.current_level, reference.current_level, true);
                    compare(states, status.target_level, reference.target_level, true);
                    compare(states, status.level_step, reference.level_step, true);
                    require(status.level_remaining == reference.level_remaining &&
                                status.pan_remaining == reference.ramp_remaining,
                            "ramp counter mismatch");
                    counters += 2;
                    for (std::size_t c = 0; c < channels; ++c) {
                        compare(states, gains[c], reference.current_gains[c], true);
                        compare(states, gains[channels + c], reference.target_gains[c], true);
                        compare(
                            states, gains[(static_cast<std::size_t>(channels) * 2) + c], reference.gain_steps[c], true);
                    }
                }
            }
            original = std::move(after);
        }
    }
    auto route = take(dsp::LiveVbapMixer::create(1, 2));
    const std::array<float, 3> input{0.25F, -0.5F, 1.0F};
    const std::array planes{MradmLiveVbapPlane{input.data(), input.size(), 1, 0}};
    const std::array initial{command(0, 0, 0, 3, 0, 0.5F)};
    std::array<float, 8> output{};
    done(route.process(3, planes, initial, {}, std::array{0.0F, 1.0F}, output));
    const std::array<float, 8> expected{0, 0.125F, 0, -0.25F, 0, 0.5F, 0, 0};
    for (std::size_t i = 0; i < output.size(); ++i) {
        compare(pcm, output.at(i), expected.at(i), true);
    }
}
ObjectState state(float gain = 1.0F) {
    ObjectState result;
    result.valid_fields = k_known_state_fields;
    result.linear_gain = gain;
    result.x = -0.3F;
    result.y = 0.8F;
    result.z = 0.1F;
    return result;
}
ElementDescriptor descriptor(uint64_t id, ElementRole role, std::string label = {}) {
    ElementDescriptor result;
    result.element_id = id;
    result.role = role;
    result.speaker_label = std::move(label);
    return result;
}
MetadataUpdate update(uint64_t id, uint32_t offset, uint64_t changed, ObjectState target, uint32_t duration = 0) {
    MetadataUpdate result;
    result.element_id = id;
    result.offset_samples = offset;
    result.changed_fields = changed;
    result.state = target;
    result.ramp_duration_samples = duration;
    return result;
}
Frame frame(uint64_t generation, uint32_t frames, std::span<const ElementDescriptor> descriptors) {
    Frame result;
    result.generation_id = generation;
    result.duration_samples = frames;
    for (const auto& element : descriptors) {
        result.pcm.push_back({element.element_id, true, std::vector<float>(frames, 0.25F)});
    }
    return result;
}
void compare_vectors(std::span<const float> a, std::span<const float> b, Stats& stats) {
    require(a.size() == b.size(), "PCM length mismatch");
    for (std::size_t i = 0; i < a.size(); ++i) {
        compare(stats, a[i], b[i]);
    }
}
void renderer_reference(Stats& pcm) {
    const std::array elements{descriptor(91, ElementRole::object),
                              descriptor(7, ElementRole::direct_speaker, "M+030"),
                              descriptor(33, ElementRole::lfe, "LFE1")};
    for (const auto* layout : {"0+2+0", "0+5+0", "4+5+0", "9.1.6", "9+10+3"}) {
        RendererConfig config;
        config.output_layout = layout;
        config.object_smoothing_frames = 19;
        config.lfe_routing_mode =
            std::string_view(layout) == "9+10+3" ? LfeRoutingMode::split_power : LfeRoutingMode::direct;
        std::vector<live_scene::Diagnostic> old_diagnostics;
        std::vector<live_scene::Diagnostic> diagnostics;
        auto before = take(live_vbap_legacy::create_live_vbap_renderer(
            config, [&](live_scene::Diagnostic d) { old_diagnostics.push_back(std::move(d)); }));
        auto after = take(
            create_live_vbap_renderer(config, [&](live_scene::Diagnostic d) { diagnostics.push_back(std::move(d)); }));
        done(before->configure_generation(0, elements));
        done(after->configure_generation(0, elements));
        for (uint32_t index = 0; index < 28; ++index) {
            const uint32_t frames = std::array<uint32_t, 7>{0, 1, 31, 33, 511, 1024, 17}.at(index % 7);
            auto f = frame(0, frames, elements);
            if (index % 5 == 0) {
                f.initial_states.push_back({91, state(0.75F)});
            }
            if (index % 4 == 0) {
                f.pcm[1].has_signal = false;
                f.pcm[1].samples.clear();
            }
            if (index % 6 == 0) {
                f.pcm.erase(f.pcm.begin() + 2);
            }
            if (frames != 0) {
                auto target = state(0.25F);
                target.x = 0.7F;
                target.diffuse = 0.5F;
                target.width = 0.4F;
                target.divergence = 0.3F;
                target.active = index % 3 != 0;
                target.channel_lock = index % 4 == 0;
                target.screen_reference = index % 3 == 0;
                f.updates.push_back(update(91,
                                           0,
                                           state_active | state_linear_gain | state_position | state_diffuse |
                                               state_extent | state_divergence | state_channel_lock |
                                               state_screen_reference,
                                           target,
                                           2100));
                f.updates.push_back(update(91, frames / 2, state_head_locked, state(), 0));
                f.updates.push_back(update(91, frames / 2, state_linear_gain, state(0.125F), 7));
                auto direct = state();
                direct.x = 0.2F;
                direct.y = 0.9F;
                auto move = update(7, frames / 2, state_position, direct, 41);
                if (index % 2 == 0) {
                    move.changed_fields = 0;
                    move.cleared_fields = state_position;
                }
                f.updates.push_back(move);
                auto jump = update(91, frames - 1, state_linear_gain, state(0.5F));
                jump.jump_position = true;
                f.updates.push_back(jump);
            }
            std::vector<float> a(static_cast<std::size_t>(frames + 1) * after->output_channels(), 17.0F);
            auto b = a;
            done(before->render(f, a));
            done(after->render(f, b));
            compare_vectors(a, b, pcm);
        }
        require(diagnostics.size() == old_diagnostics.size(), "diagnostic count differs");
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            require(diagnostics[i].code == old_diagnostics[i].code &&
                        diagnostics[i].element_id == old_diagnostics[i].element_id &&
                        diagnostics[i].field_mask == old_diagnostics[i].field_mask &&
                        diagnostics[i].message == old_diagnostics[i].message,
                    "diagnostic order/content differs");
        }
        require(after->tail_input_frames() == 0, "VBAP acquired a tail");
    }
}
void atomic_renderer_errors(Stats& pcm) {
    RendererConfig config;
    config.output_layout = "0+5+0";
    config.object_smoothing_frames = 17;
    const std::array elements{descriptor(91, ElementRole::object)};
    auto a = take(create_live_vbap_renderer(config, {}));
    auto b = take(create_live_vbap_renderer(config, {}));
    done(a->configure_generation(0, elements));
    done(b->configure_generation(0, elements));
    auto seed = frame(0, 3, elements);
    seed.initial_states.push_back({91, state()});
    seed.updates.push_back(update(91, 1, state_linear_gain, state(0.1F), 30));
    std::vector<float> x(18, 17.0F);
    auto y = x;
    done(a->render(seed, x));
    done(b->render(seed, y));
    auto invalid = frame(0, 3, elements);
    invalid.updates = {update(91, 0, state_linear_gain, state(0.9F), 8), update(999, 2, state_linear_gain, state())};
    x.assign(18, 17.0F);
    require(!a->render(invalid, x), "late invalid element accepted");
    require(std::ranges::all_of(x, [](float v) { return v == 17.0F; }), "failed frame cleared output");
    auto next = frame(0, 3, elements);
    done(a->render(next, x));
    done(b->render(next, y));
    compare_vectors(x, y, pcm);
    const std::array duplicate{elements.front(), elements.front()};
    require(!a->configure_generation(1, duplicate), "duplicate generation accepted");
    next.generation_id = 1;
    x.assign(18, 17.0F);
    require(!a->render(next, x), "failed generation replaced old one");
    next.generation_id = 0;
    require(!a->render(next, std::span{x}.first(17)), "short output accepted");
    next.pcm.front().samples.resize(2);
    require(!a->render(next, x), "short plane accepted");
    next.pcm.front().samples.resize(3, 0.25F);
    done(a->render(next, x));
    done(b->render(next, y));
    compare_vectors(x, y, pcm);
    // Valid policy-expanded positions need not stay inside the producer's canonical cube.
    next.initial_states.push_back({91, state()});
    next.initial_states.front().state.x = 2.0F;
    done(a->render(next, x));
    done(b->render(next, y));
    compare_vectors(x, y, pcm);
    (*a).reset();
    (*b).reset();
    done(a->configure_generation(0, {}));
    done(b->configure_generation(0, {}));
    auto empty = frame(0, 3, {});
    x.assign(18, 17.0F);
    done(a->render(empty, x));
    require(std::ranges::all_of(x, [](float v) { return v == 0.0F; }), "empty generation is not silent");

    const std::array routing{descriptor(91, ElementRole::object),
                             descriptor(7, ElementRole::direct_speaker, "unknown-label")};
    std::vector<live_scene::Diagnostic> diagnostics;
    auto subject =
        take(create_live_vbap_renderer(config, [&](live_scene::Diagnostic d) { diagnostics.push_back(std::move(d)); }));
    auto control = take(create_live_vbap_renderer(config, {}));
    done(subject->configure_generation(0, routing));
    done(control->configure_generation(0, routing));
    auto bad = frame(0, 3, routing);
    auto warning = state(0.25F);
    warning.diffuse = 0.5F;
    bad.initial_states.push_back({91, warning});
    x.assign(18, 17.0F);
    require(!subject->render(bad, x), "unsupported route accepted");
    require(diagnostics.empty() && std::ranges::all_of(x, [](float v) { return v == 17.0F; }),
            "failed routing published warnings/output");
    auto recover = frame(0, 3, routing);
    recover.initial_states.push_back({7, state()});
    done(subject->render(recover, x));
    done(control->render(recover, y));
    compare_vectors(x, y, pcm);
    const auto before_warning = diagnostics.size();
    recover.initial_states.push_back({91, warning});
    done(subject->render(recover, x));
    require(diagnostics.size() == before_warning + 1, "failed frame consumed warn_once state");
}
void print_stats(std::ostream& out, const Stats& s) {
    out << R"({"values":)" << s.values << R"(,"differing_values":)" << s.differing << R"(,"max_absolute_error":)"
        << s.max_error << '}';
}
} // namespace
int main(int argc, char** argv) {
    try {
        Stats numerical;
        Stats states;
        Stats renderer;
        uint64_t counters = 0;
        numeric_targets(numerical, states, counters);
        renderer_reference(renderer);
        atomic_renderer_errors(renderer);
        if (argc > 1) {
            std::ofstream out(argv[1]);
            require(out.good(), "cannot open report");
            out << std::setprecision(17) << R"({"baseline":"577fe2d","passed":true,"numerical_pcm":)";
            print_stats(out, numerical);
            out << R"(,"exact_state":)";
            print_stats(out, states);
            out << R"(,"renderer_pcm":)";
            print_stats(out, renderer);
            out << R"(,"exact_counters":)" << counters << "}\n";
        }
        std::cout << "Live VBAP comparison passed: " << numerical.values + states.values + renderer.values
                  << " float values; " << counters << " counters\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
