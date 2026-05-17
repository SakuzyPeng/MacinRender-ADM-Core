#include <cstdlib>
#include <iostream>

#include "adm/render.h"

int main() {
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
