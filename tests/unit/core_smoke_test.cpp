#include <cstdlib>
#include <iostream>

#include "adm/render.h"
#include "adm/version.h"

int main() {
    if (mradm::version().empty()) {
        std::cerr << "version should not be empty\n";
        return EXIT_FAILURE;
    }

    mradm::RenderService service;
    mradm::RenderRequest request;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;

    mradm::RenderResult result = service.render(request, progress, logs);
    if (result.error.code != mradm::ErrorCode::invalid_argument) {
        std::cerr << "empty input should return invalid_argument\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
