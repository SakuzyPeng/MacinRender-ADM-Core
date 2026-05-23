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

// Inline measurement results from the renderer's render loop.
// Both fields are nullopt when the signal is silence or too short for gating.
// For HOA outputs these are coefficient-domain diagnostics, not playback loudness.
struct RenderMetrics {
    std::optional<double> measured_lufs;      // BS.1770-4 integrated loudness (LUFS)
    std::optional<double> measured_peak_dbtp; // ITU-R BS.1770-4 True Peak (dBTP)
};

struct RenderResult {
    Error error;
    std::optional<std::filesystem::path> output_path;
    std::optional<RenderMetrics> metrics;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool success() const noexcept { return error.ok(); }
};

// Input to a renderer backend: file paths, output layout, and pre-parsed scene metadata.
struct RenderPlan {
    std::string input_path;
    std::string output_path;
    std::string output_layout;
    AdmScene scene;                                 // populated by RenderService; backends must not re-parse ADM
    std::optional<std::filesystem::path> sofa_path; // binaural renderer only; empty = built-in KEMAR
    // Default gain-interpolation ramp in milliseconds (from RenderOptions).
    uint32_t default_interp_ms{5};
    // Renderer-side control-rate smoothing for dense Objects metadata updates.
    // 0 disables smoothing.
    uint32_t object_smoothing_frames{8875};
};

// Abstract renderer backend interface.
class IRenderer {
  public:
    virtual ~IRenderer() = default;
    [[nodiscard]] virtual CapabilityReport capabilities() const = 0;
    [[nodiscard]] virtual Result<RenderMetrics>
    render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) = 0;
};

class RenderService {
  public:
    RenderService();

    [[nodiscard]] RenderResult render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const;
};

} // namespace mradm
