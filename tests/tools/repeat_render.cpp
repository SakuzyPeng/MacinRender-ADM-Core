// Release-only measurement: two full renders in one process.
// Differences are recorded by render-matrix.sh; this tool only fails on rendering errors.
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/render.h"

#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
extern "C" void mr_adm_diagnostic_seed(unsigned int seed);
#endif

namespace {
class MeasurementLog final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel /*level*/, std::string_view module, std::string_view message) override {
        if (module == "binaural") {
            std::cout << message << "\n";
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv, argv + argc);
    if (args.size() < 3U) {
        std::cerr << "usage: mr_adm_repeat_render <input.wav> <output-prefix> [--reset-rng] [--cloud]\n";
        return 2;
    }
    bool reset_rng = false;
    bool cloud = false;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--reset-rng") {
            reset_rng = true;
        } else if (args[i] == "--cloud") {
            cloud = true;
        } else {
            std::cerr << "unknown option: " << args[i] << "\n";
            return 2;
        }
    }
    mradm::RenderService service;
    mradm::NullProgressSink progress;
    MeasurementLog logs;
    mradm::RenderRequest request;
    request.input_path = std::string{args[1]};
    request.options.renderer = mradm::RendererSelection::saf_binaural;
    request.options.output_layout = "binaural";
    request.options.output_bit_depth = mradm::OutputBitDepth::f32;
    request.options.peak_limit = false;
    request.options.binaural_spread_mode =
        cloud ? mradm::BinauralSpreadMode::cloud : mradm::BinauralSpreadMode::saf_spreader;
    std::cout << "hardware_concurrency=" << std::thread::hardware_concurrency() << "\n";
    for (int pass = 1; pass <= 2; ++pass) {
        // Diagnostic intervention only: the C RNG algorithm still differs between runtimes,
        // and process-global reseeding is unsuitable for concurrent production rendering.
        if (reset_rng) {
            std::srand(1);
#ifdef MR_ADM_DIAGNOSTIC_PORTABLE_RNG
            mr_adm_diagnostic_seed(1);
#endif
        }
        request.output_path = std::string{args[2]} + "-" + std::to_string(pass) + ".wav";
        std::cout << "same_process_pass=" << pass << "\n";
        const auto result = service.render(request, progress, logs);
        if (!result.success()) {
            std::cerr << result.error.message << "\n";
            return 2;
        }
    }
    return 0;
}
