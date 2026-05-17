#pragma once

#include <optional>
#include <string>
#include <vector>

#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/progress.h"

namespace mradm {

struct Diagnostic {
    LogLevel level{LogLevel::info};
    std::string message;
};

struct RenderResult {
    Error error;
    std::optional<std::filesystem::path> output_path;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool success() const noexcept { return error.ok(); }
};

class RenderService {
  public:
    RenderService();

    [[nodiscard]] RenderResult render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const;
};

} // namespace mradm
