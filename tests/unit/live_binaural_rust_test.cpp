#include <bit>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

#include "../reference/scene_numeric/live_binaural.h"
#include "live_binaural_renderer.h"
namespace {
using namespace mradm;
using namespace mradm::live_scene;
class LiveBinauralChecks {
    size_t compared = 0;
    double maximum = 0;
    int failures = 0;
    void check(bool good, std::string_view what) {
        if (!good) {
            if (failures < 25) {
                std::cerr << "FAIL " << what << '\n';
            }
            ++failures;
        }
    }
    template <class T> static T take(Result<T> value) {
        if (!value) {
            throw std::runtime_error(value.error().message);
        }
        return std::move(*value);
    }
    static void require(Result<void> value) {
        if (!value) {
            throw std::runtime_error(value.error().message);
        }
    }
    void compare(std::span<const float> a, std::span<const float> b, bool exact = false) {
        check(a.size() == b.size(), "output length");
        for (size_t i = 0; i < a.size(); ++i) {
            ++compared;
            const auto error = std::abs(double(a[i]) - double(b[i]));
            maximum = std::max(maximum, error);
            check(exact ? std::bit_cast<uint32_t>(a[i]) == std::bit_cast<uint32_t>(b[i])
                        : error <= 2e-6 + (2e-6 * std::abs(double(b[i]))),
                  "PCM agreement");
        }
    }
    static RendererConfig config(BinauralSpreadMode spread) {
        RendererConfig c;
        c.renderer = RendererSelection::saf_binaural;
        c.output_layout = "binaural";
        c.binaural_spread_mode = spread;
        c.object_smoothing_frames = 733;
        return c;
    }
    static std::vector<ElementDescriptor> descriptors() {
        ElementDescriptor a;
        a.element_id = 1;
        a.role = ElementRole::object;
        ElementDescriptor b;
        b.element_id = 2;
        b.role = ElementRole::direct_speaker;
        b.speaker_label = "unknown";
        b.has_position = true;
        b.x = 0.3F;
        b.y = 1;
        b.z = 0.2F;
        ElementDescriptor c;
        c.element_id = 3;
        c.role = ElementRole::lfe;
        return {a, b, c};
    }
    static Frame frame(uint32_t frames, uint64_t start = 0) {
        Frame f;
        f.generation_id = 7;
        f.epoch_id = 3;
        f.duration_samples = frames;
        f.media_sample_start = static_cast<int64_t>(start);
        for (uint64_t id = 1; id <= 3; ++id) {
            PcmPlane p;
            p.element_id = id;
            p.has_signal = true;
            p.samples.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                p.samples[i] = (float(((i * 37) + start + (id * 11)) % 97) * 0.0005F) - 0.02F;
            }
            f.pcm.push_back(std::move(p));
        }
        return f;
    }
    static bool same_diagnostics(const std::vector<Diagnostic>& a, const std::vector<Diagnostic>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].code != b[i].code || a[i].field_mask != b[i].field_mask || a[i].element_id != b[i].element_id ||
                a[i].message != b[i].message) {
                return false;
            }
        }
        return true;
    }
    // NOLINTNEXTLINE(readability-function-size): the ordered event matrix is shared with the frozen renderer.
    void legacy_comparison() {
        for (const auto spread :
             {BinauralSpreadMode::automatic, BinauralSpreadMode::none, BinauralSpreadMode::saf_spreader}) {
            std::vector<Diagnostic> diagnostics;
            std::vector<Diagnostic> reference_diagnostics;
            auto actual = take(create_live_binaural_renderer(
                config(spread), [&](Diagnostic d) { diagnostics.push_back(std::move(d)); }));
            auto reference = take(scene_live_reference::create_live_binaural_renderer(
                config(spread), [&](Diagnostic d) { reference_diagnostics.push_back(std::move(d)); }));
            const auto desc = descriptors();
            require(actual->configure_generation(7, desc));
            require(reference->configure_generation(7, desc));
            uint64_t start = 0;
            uint32_t iteration = 0;
            for (const uint32_t frames :
                 {0U, 1U, 31U, 32U, 33U, 511U, 512U, 513U, 1023U, 1024U, 1301U, 2049U, 17U, 1024U, 1024U}) {
                auto f = frame(frames, start);
                if (iteration == 0) {
                    ObjectState state;
                    state.valid_fields = k_known_state_fields;
                    state.linear_gain = 0.75F;
                    state.diffuse = 0.4F;
                    state.width = 0.3F;
                    state.height = 0.2F;
                    state.depth = 0.1F;
                    state.divergence = 0.5F;
                    state.screen_reference = true;
                    f.initial_states.push_back({1, state});
                }
                if (frames > 0 && iteration < 11) {
                    MetadataUpdate e;
                    e.element_id = 1;
                    e.state.valid_fields = k_known_state_fields;
                    e.changed_fields =
                        state_position | state_linear_gain | state_extent | state_diffuse | state_divergence;
                    e.ramp_duration_samples = iteration % 2 == 0 ? 513 : 0;
                    e.state.x = float(int(iteration % 5) - 2) * 0.3F;
                    e.state.y = 0.6F;
                    e.state.z = float(iteration % 3) * 0.1F;
                    e.state.linear_gain = 0.25F + (float(iteration % 4) * 0.1F);
                    e.state.width = float(iteration % 4) * 0.15F;
                    e.state.height = 0.1F;
                    e.state.depth = 0.05F;
                    e.state.diffuse = float(iteration % 3) * 0.3F;
                    e.state.divergence = 0.3F;
                    f.updates.push_back(e);
                    e.offset_samples = frames / 2;
                    e.changed_fields = state_head_locked;
                    e.state.head_locked = iteration % 2 != 0;
                    f.updates.push_back(e);
                    e.element_id = 2;
                    e.changed_fields = state_position;
                    e.state.x = -0.4F;
                    e.state.y = 0.7F;
                    e.state.z = 0.2F;
                    e.ramp_duration_samples = 31;
                    f.updates.push_back(e);
                    if (iteration % 3 == 0) {
                        e.changed_fields = 0;
                        e.cleared_fields = state_position;
                        f.updates.push_back(e);
                    }
                }
                if (iteration >= 11) {
                    for (auto& p : f.pcm) {
                        p.has_signal = false;
                    }
                    if (iteration == 12) {
                        f.pcm.clear();
                    }
                }
                const ListenerOrientation pose{float(iteration) * 3.0F, 10.0F, -7.0F};
                actual->set_listener_orientation(pose);
                reference->set_listener_orientation(pose);
                std::vector<float> output((size_t(frames) * 2) + 3, 17);
                std::vector<float> old(output);
                require(actual->render(f, output));
                require(reference->render(f, old));
                compare(output, old);
                check(same_diagnostics(diagnostics, reference_diagnostics), "diagnostic order and deduplication");
                start += frames;
                ++iteration;
            }
            check(actual->tail_input_frames() == reference->tail_input_frames(), "tail length");
        }
    }
    // NOLINTNEXTLINE(readability-function-size): verify recovery against one continuous untouched control instance.
    void recovery() {
        std::vector<Diagnostic> logs;
        auto actual = take(create_live_binaural_renderer(config(BinauralSpreadMode::none),
                                                         [&](Diagnostic d) { logs.push_back(std::move(d)); }));
        auto control = take(create_live_binaural_renderer(config(BinauralSpreadMode::none), {}));
        const auto desc = descriptors();
        require(actual->configure_generation(7, desc));
        require(control->configure_generation(7, desc));
        std::vector<float> out(1024);
        std::vector<float> expected(1024);
        auto good = frame(512);
        require(actual->render(good, out));
        require(control->render(good, expected));
        compare(out, expected, true);
        const auto log_count = logs.size();
        auto bad = frame(512, 512);
        MetadataUpdate event;
        event.element_id = 1;
        event.changed_fields = state_linear_gain | state_extent;
        event.state.valid_fields = event.changed_fields;
        event.state.linear_gain = 0.25F;
        event.state.width = 1;
        bad.updates.push_back(event);
        event.element_id = 999;
        event.offset_samples = 511;
        bad.updates.push_back(event);
        out.assign(1024, 17);
        check(!actual->render(bad, out), "late invalid event rejected");
        check(std::ranges::all_of(out, [](float x) { return x == 17; }), "rejected output untouched");
        // The renderer callback can append to logs; cppcheck cannot follow it through the interface.
        // cppcheck-suppress knownConditionTrueFalse
        check(logs.size() == log_count, "failed diagnostics untouched");
        bad.updates.resize(1);
        bad.updates[0].state.linear_gain = std::numeric_limits<float>::max();
        bad.updates[0].jump_position = true;
        bad.updates[0].offset_samples = 511;
        bad.pcm[0].samples.back() = std::numeric_limits<float>::max();
        check(!actual->render(bad, out), "late DSP overflow rejected before history changes");
        check(std::ranges::all_of(out, [](float x) { return x == 17; }), "numerical rejection output untouched");
        auto duplicate = desc;
        duplicate.push_back(desc.front());
        check(!actual->configure_generation(8, duplicate), "failed reconfiguration preserves generation");
        good = frame(512, 512);
        require(actual->render(good, out));
        require(control->render(good, expected));
        compare(out, expected, true);
        check(!actual->render(good, std::span<float>{out}.first(1023)), "short output rejected");
        good = frame(512, 1024);
        require(actual->render(good, out));
        require(control->render(good, expected));
        compare(out, expected, true);
        (*actual).reset();
        (*control).reset();
        Frame empty;
        empty.duration_samples = 512;
        require(actual->render(empty, out));
        require(control->render(empty, expected));
        compare(out, expected, true);
        require(actual->configure_generation(7, desc));
        require(control->configure_generation(7, desc));
        good = frame(512);
        require(actual->render(good, out));
        require(control->render(good, expected));
        compare(out, expected, true);
        ElementDescriptor invalid;
        invalid.element_id = 2;
        invalid.role = ElementRole::direct_speaker;
        invalid.speaker_label = "missing";
        require(actual->configure_generation(7, std::span{&invalid, 1}));
        Frame repair;
        repair.generation_id = 7;
        repair.duration_samples = 1;
        out.assign(2, 29);
        check(!actual->render(repair, out) && out == std::vector<float>(2, 29), "unroutable initial state is atomic");
        ObjectState positioned;
        positioned.valid_fields = state_position;
        repair.initial_states.push_back({2, positioned});
        require(actual->render(repair, out));
        check(out == std::vector<float>(2, 0), "route failure recovery");
        auto routed_control = take(create_live_binaural_renderer(config(BinauralSpreadMode::none), {}));
        require(routed_control->configure_generation(7, std::span{&invalid, 1}));
        std::vector<float> routed_output(2);
        require(routed_control->render(repair, routed_output));
        Frame failed_route = repair;
        failed_route.duration_samples = 2;
        failed_route.initial_states.clear();
        MetadataUpdate clear;
        clear.element_id = 2;
        clear.offset_samples = 1;
        clear.cleared_fields = state_position;
        failed_route.updates.push_back(clear);
        out.assign(4, 29);
        check(!actual->render(failed_route, out) && out == std::vector<float>(4, 29), "late route error is atomic");
        repair.duration_samples = 2;
        repair.initial_states.clear();
        repair.pcm.push_back({2, true, {0.125F, -0.125F}});
        routed_output.resize(4);
        require(actual->render(repair, out));
        require(routed_control->render(repair, routed_output));
        compare(out, routed_output, true);
    }

  public:
    int run(int argc, char** argv) {
        try {
            legacy_comparison();
            recovery();
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Live binaural reference: " << compared << " floats; max absolute error " << maximum
                  << "; failures " << failures << '\n';
        if (argc > 1) {
            std::ofstream json{argv[1]};
            json << std::setprecision(17) << "{\"compared\":" << compared << ",\"max_absolute_error\":" << maximum
                 << ",\"failures\":" << failures << "}\n";
        }
        return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
};
} // namespace
int main(int argc, char** argv) {
    return LiveBinauralChecks{}.run(argc, argv);
}
