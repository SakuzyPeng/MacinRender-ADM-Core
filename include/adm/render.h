#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adm/capability.h"
#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/progress.h"
#include "adm/scene.h"

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

// Input to a renderer backend: file paths, output layout, and pre-parsed scene metadata.
struct RenderPlan {
    std::string input_path;
    std::string output_path;
    std::string output_layout;
    AdmScene scene; // populated by RenderService; backends must not re-parse ADM
    // Default gain-interpolation ramp in milliseconds (from RenderOptions).
    uint32_t default_interp_ms{5};
};

// Abstract renderer backend interface.
class IRenderer {
  public:
    virtual ~IRenderer() = default;
    [[nodiscard]] virtual CapabilityReport capabilities() const = 0;
    [[nodiscard]] virtual Result<void> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) = 0;
};

class RenderService {
  public:
    RenderService();

    [[nodiscard]] RenderResult render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const;
};

} // namespace mradm
