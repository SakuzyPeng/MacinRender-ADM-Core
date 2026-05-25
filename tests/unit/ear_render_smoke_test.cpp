#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "adm/render.h"
#include "adm/render_ear.h"

int main() {
    const auto caps = mradm::ear_capabilities();
    const auto wav71 = std::ranges::find_if(caps.supported_layouts, [](const auto& layout) {
        return layout.id == "wav71" && layout.channel_count == 8U && layout.lfe_count == 1U;
    });
    if (wav71 == caps.supported_layouts.end()) {
        std::cerr << "libear capabilities must expose wav71 as the public 7.1 layout\n";
        return EXIT_FAILURE;
    }
    const auto old_070 =
        std::ranges::find_if(caps.supported_layouts, [](const auto& layout) { return layout.id == "0+7+0"; });
    if (old_070 != caps.supported_layouts.end()) {
        std::cerr << "libear capabilities must not expose old 0+7+0 layout id\n";
        return EXIT_FAILURE;
    }
    const auto expected_layouts =
        std::array{"0+2+0", "0+5+0", "2+5+0", "4+5+0", "wav71", "4+7+0", "4+5+4", "9.1.6", "9+10+3"};
    for (const auto* expected : expected_layouts) {
        const auto it = std::ranges::find_if(caps.supported_layouts,
                                             [expected](const auto& layout) { return layout.id == expected; });
        if (it == caps.supported_layouts.end()) {
            std::cerr << "libear capabilities missing layout " << expected << "\n";
            return EXIT_FAILURE;
        }
    }

    mradm::RenderRequest request;
    request.input_path = "/tmp/nonexistent_mr_ear_test_xyz.wav";
    request.output_path = "/tmp/mr_ear_test_out_xyz.wav";

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);

    if (result.success()) {
        std::cerr << "expected error for nonexistent file, got success\n";
        return EXIT_FAILURE;
    }
    if (result.error.code != mradm::ErrorCode::io_error) {
        std::cerr << "expected io_error, got code " << static_cast<int>(result.error.code) << "\n";
        return EXIT_FAILURE;
    }
    if (result.error.message.empty()) {
        std::cerr << "error message must not be empty\n";
        return EXIT_FAILURE;
    }

    std::cout << "ear_render smoke test passed: " << result.error.message << "\n";
    return EXIT_SUCCESS;
}
