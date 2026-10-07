#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bed.h"
#include "live_triple_balance_renderer.h"

namespace {
using namespace mradm;
using namespace mradm::live_scene;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename T> T take(Result<T> value) {
    if (!value) {
        throw std::runtime_error(value.error().message + ": " + value.error().context);
    }
    return std::move(*value);
}
void done(Result<void> value) {
    if (!value) {
        throw std::runtime_error(value.error().message + ": " + value.error().context);
    }
}
RendererConfig config(const std::string& layout) {
    RendererConfig result;
    result.renderer = RendererSelection::triple_balance;
    result.output_layout = layout;
    return result;
}
std::unique_ptr<ILiveSceneRenderer> object_renderer(const RendererConfig& settings, DiagnosticSink sink = {}) {
    auto renderer = take(create_live_triple_balance_renderer(settings, std::move(sink)));
    ElementDescriptor descriptor;
    descriptor.element_id = 7;
    done(renderer->configure_generation(1, std::span{&descriptor, 1}));
    return renderer;
}
Frame frame(uint32_t count = 73) {
    Frame result;
    result.epoch_id = 1;
    result.generation_id = 1;
    result.flags = frame_state_complete;
    result.duration_samples = count;
    result.pcm.push_back({7, true, std::vector<float>(count, .125F)});
    ObjectState state;
    state.valid_fields = k_known_state_fields;
    state.x = -.5F;
    state.y = .5F;
    state.z = .5F;
    result.initial_states.push_back({7, state});
    return result;
}
void bed_routes() {
    constexpr std::array<const char*, 10> labels{
        "M+030", "RC_R", "C", "LFE", "Lss", "M-090", "RC_Lrs", "Rb", "TSL", "U-090"};
    constexpr std::array<const char*, 10> room{
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts"};
    for (const auto& layout : {"7.1.4", "9.1.6", "22.2"}) {
        for (const auto mode : {LfeRoutingMode::direct, LfeRoutingMode::split_power}) {
            auto settings = config(layout);
            settings.lfe_routing_mode = mode;
            auto renderer = take(create_live_triple_balance_renderer(settings, {}));
            std::vector<ElementDescriptor> descriptors;
            for (std::size_t i = 10; i-- > 0;) {
                ElementDescriptor descriptor;
                descriptor.element_id = 100 + i;
                descriptor.role = i == 3 ? ElementRole::lfe : ElementRole::direct_speaker;
                descriptor.speaker_label = labels.at(i);
                descriptors.push_back(descriptor);
            }
            done(renderer->configure_generation(1, descriptors));
            Frame input;
            input.generation_id = 1;
            input.duration_samples = 10;
            std::vector<float> expected(std::size_t{10} * renderer->output_channels(), 0);
            for (std::size_t i = 0; i < 10; ++i) {
                PcmPlane plane;
                plane.element_id = 100 + i;
                plane.has_signal = true;
                plane.samples.assign(10, 0);
                plane.samples[i] = 1;
                input.pcm.push_back(std::move(plane));
                SceneDirectSpeakersBlock block;
                block.speaker_labels = {room.at(i)};
                const auto gains = take(triple_balance::bed_gains(block, layout, mode));
                std::ranges::copy(gains, expected.begin() + static_cast<std::ptrdiff_t>(i * gains.size()));
            }
            std::vector<float> actual(expected.size());
            done(renderer->render(input, actual));
            require(actual == expected, "unordered Scene bed must reproduce fixed routing and LFE split");
            auto incomplete = descriptors;
            incomplete.pop_back();
            require(!renderer->configure_generation(2, incomplete), "incomplete bed rejected");
            auto duplicate = descriptors;
            duplicate[0].speaker_label = duplicate[1].speaker_label;
            require(!renderer->configure_generation(2, duplicate), "duplicate bed label rejected");
            done(renderer->render(input, actual));
            require(actual == expected, "failed generation preserves previous renderer");
            ObjectState moved;
            moved.valid_fields = state_position;
            input.initial_states = {{100, moved}};
            actual.assign(actual.size(), 7);
            require(!renderer->render(input, actual), "bed position override rejected");
            require(std::ranges::all_of(actual, [](float x) { return x == 7; }), "bed rejection is atomic");
        }
    }
}
void descriptor_position_overrides() {
    for (const auto* layout : {"7.1.4", "9.1.6"}) {
        const auto settings = config(layout);
        auto renderer = take(create_live_triple_balance_renderer(settings, {}));
        ElementDescriptor descriptor;
        descriptor.element_id = 7;
        descriptor.has_position = true;
        descriptor.z = -.5F;
        done(renderer->configure_generation(1, std::span{&descriptor, 1}));

        auto input = frame();
        input.initial_states.clear();
        std::vector<float> actual(static_cast<std::size_t>(input.duration_samples) * renderer->output_channels(), 7);
        const auto rejected = renderer->render(input, actual);
        require(!rejected && rejected.error().code == ErrorCode::unsupported,
                "unsupported descriptor position is rejected when it remains effective");
        require(std::ranges::all_of(actual, [](float value) { return value == 7; }),
                "default position rejection preserves output");

        input = frame();
        done(renderer->render(input, actual));
        auto reference = object_renderer(settings);
        std::vector<float> expected(actual.size());
        done(reference->render(input, expected));
        require(actual == expected, "initial position overrides the descriptor before backend validation");
    }
}
void unsupported_states() {
    for (int kind = 0; kind < 9; ++kind) {
        auto renderer = object_renderer(config("7.1.4"));
        auto valid = frame();
        auto invalid = valid;
        auto& state = invalid.initial_states.front().state;
        switch (kind) {
        case 0:
            state.width = .3F;
            break;
        case 1:
            state.diffuse = .5F;
            break;
        case 2:
            state.diffuse = 1;
            break;
        case 3:
            state.divergence = .2F;
            break;
        case 4:
            state.channel_lock = true;
            break;
        case 5:
            state.screen_reference = true;
            break;
        case 6:
            state.head_locked = true;
            break;
        case 7:
            state.z = -.1F;
            break;
        default:
            state.x = 1.1F;
            break;
        }
        std::vector<float> actual((876U), 7);
        const auto rejected = renderer->render(invalid, actual);
        require(!rejected && rejected.error().code == ErrorCode::unsupported, "unsupported state needs explicit error");
        require(rejected.error().context.find("element=7") != std::string::npos, "rejection identifies element");
        require(std::ranges::all_of(actual, [](float x) { return x == 7; }), "rejected state leaves output untouched");
        done(renderer->render(valid, actual));
        auto reference = object_renderer(config("7.1.4"));
        std::vector<float> expected(actual.size());
        done(reference->render(valid, expected));
        require(actual == expected, "failed call does not mutate history");
    }
    auto settings = config("7.1.4");
    settings.speaker_spread_mode = SpeakerSpreadMode::none;
    std::vector<Diagnostic> diagnostics;
    auto renderer = object_renderer(settings, [&](Diagnostic value) { diagnostics.push_back(std::move(value)); });
    auto input = frame();
    input.initial_states.front().state.width = .4F;
    input.initial_states.front().state.diffuse = .3F;
    std::vector<float> output((876U));
    done(renderer->render(input, output));
    done(renderer->render(input, output));
    require(diagnostics.size() == 1 && diagnostics.front().code == DiagnosticCode::semantic_degraded,
            "spread=none explicitly reports ignored fields once");
    settings.sample_rate = 96000;
    settings.speaker_spread_mode = SpeakerSpreadMode::automatic;
    renderer = object_renderer(settings);
    input.initial_states.front().state.diffuse = 0;
    input.initial_states.front().state.height = input.initial_states.front().state.depth = .4F;
    require(!renderer->render(input, output), "nonzero size requires 48 kHz");
    for (int kind = 0; kind < 4; ++kind) {
        auto bad = config("7.1.4");
        if (kind == 0) {
            bad.output_layout = "5.1";
        }
        if (kind == 1) {
            bad.speaker_geometry = SpeakerGeometry::apple;
        }
        if (kind == 2) {
            bad.speaker_spread_mode = SpeakerSpreadMode::mdap;
        }
        if (kind == 3) {
            bad.output_layout = "22.2";
            bad.sample_rate = 96000;
        }
        require(!create_live_triple_balance_renderer(bad, {}), "unsupported config rejected");
    }
}
void independent_ramp_deadlines() {
    auto settings = config("9.1.6");
    settings.object_smoothing_frames = 32;
    auto input = frame(700);
    auto& initial = input.initial_states.front().state;
    initial.width = initial.height = initial.depth = .3F;
    ObjectState size = initial;
    size.width = size.height = size.depth = .7F;
    input.updates.push_back({7, 17, 151, false, state_extent, size});
    ObjectState position = size;
    position.x = .5F;
    input.updates.push_back({7, 31, 103, false, state_position, position});
    auto renderer = object_renderer(settings);
    std::vector<float> expected((11200U));
    done(renderer->render(input, expected));
    ObjectState muted = position;
    muted.active = false;
    input.updates.push_back({7, 51, 0, true, state_active, muted});
    muted.active = true;
    input.updates.push_back({7, 211, 0, true, state_active, muted});
    renderer = object_renderer(settings);
    std::vector<float> actual(expected.size());
    done(renderer->render(input, actual));
    require(std::equal(actual.begin() + (3376U), actual.end(), expected.begin() + (3376U)),
            "mute and gain controls must not restart spatial ramps or filter history");
    require(std::ranges::all_of(std::span{actual}.subspan((816U), (2560U)), [](float v) { return v == 0; }),
            "active=false is a post-DSP mute");
}
} // namespace

int main() {
    try {
        bed_routes();
        descriptor_position_overrides();
        unsupported_states();
        independent_ramp_deadlines();
        std::cout << "Live Triple Balance semantics, routes, ramps and error atomicity passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
