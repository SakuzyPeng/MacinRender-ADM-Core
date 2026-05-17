#include <cstdlib>
#include <iostream>

#include "adm/render.h"
#include "adm/render_vbap.h"

int main() {
    bool ok = true;

    // ── Capabilities ──────────────────────────────────────────────────────────
    const auto caps = mradm::vbap_capabilities();

    if (caps.backend_name != "saf-vbap") {
        std::cerr << "FAIL: expected backend_name 'saf-vbap', got '" << caps.backend_name << "'\n";
        ok = false;
    }
    if (caps.supported_layouts.empty()) {
        std::cerr << "FAIL: supported_layouts must not be empty\n";
        ok = false;
    }
    if (!caps.supports_objects) {
        std::cerr << "FAIL: saf-vbap must declare supports_objects\n";
        ok = false;
    }

    // ── Engine routing ────────────────────────────────────────────────────────
    // Nonexistent file → io_error at scene import; proves the backend is
    // recognised by RenderService (no "renderer not available" short-circuit).
    mradm::RenderRequest request;
    request.input_path = "/tmp/nonexistent_mr_vbap_test_xyz.wav";
    request.output_path = "/tmp/mr_vbap_test_out_xyz.wav";
    request.options.renderer = mradm::RendererSelection::saf;

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const mradm::RenderResult result = service.render(request, progress, logs);

    if (result.success()) {
        std::cerr << "FAIL: expected error for nonexistent file, got success\n";
        ok = false;
    }
    // io_error comes from scene import; unsupported would mean engine rejected
    // the backend before even trying. Both are wrong outcomes here.
    if (result.error.code != mradm::ErrorCode::io_error) {
        std::cerr << "FAIL: expected io_error (scene import), got code "
                  << static_cast<int>(result.error.code)
                  << " — message: " << result.error.message << "\n";
        ok = false;
    }

    if (ok) {
        std::cout << "vbap smoke test passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
