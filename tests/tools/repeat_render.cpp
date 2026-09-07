// Release-only measurement: two full renders in one process, without resetting global RNGs.
// Differences are recorded by render-matrix.sh; this tool only fails on rendering errors.
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/render.h"

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
    if (args.size() != 3U) {
        std::cerr << "usage: mr_adm_repeat_render <input.wav> <output-prefix>\n";
        return 2;
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
    request.options.binaural_spread_mode = mradm::BinauralSpreadMode::saf_spreader;
    std::cout << "hardware_concurrency=" << std::thread::hardware_concurrency() << "\n";
    for (int pass = 1; pass <= 2; ++pass) {
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
