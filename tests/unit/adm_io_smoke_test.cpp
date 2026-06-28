#include <cstdlib>
#include <iostream>

#include "adm/io.h"

#include "test_portable.h"

int main() {
    // Non-existent file must return io_error (not throw)
    auto result = mradm::io::import_scene(mr_test::temp_prefix() + "nonexistent_adm_mr_test_xyz.wav");

    if (result.has_value()) {
        std::cerr << "expected error for nonexistent file, got success\n";
        return EXIT_FAILURE;
    }
    if (result.error().code != mradm::ErrorCode::io_error) {
        std::cerr << "expected io_error, got code " << static_cast<int>(result.error().code) << "\n";
        return EXIT_FAILURE;
    }
    if (result.error().message.empty()) {
        std::cerr << "error message must not be empty\n";
        return EXIT_FAILURE;
    }

    std::cout << "adm_io smoke test passed: " << result.error().message << "\n";
    return EXIT_SUCCESS;
}
