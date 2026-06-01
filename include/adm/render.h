#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
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
// For HOA outputs, LUFS is measured via an AllRAD 7.1.4 decode (spatial domain).
// LFE is excluded from LUFS (EBUR128_UNUSED) but tracked separately for True Peak.
struct RenderMetrics {
    std::optional<double> measured_lufs;      // BS.1770-4 integrated loudness (LUFS)
    std::optional<double> measured_peak_dbtp; // ITU-R BS.1770-4 True Peak (dBTP)
};

struct RenderResult {
    Error error;
    std::optional<std::filesystem::path> output_path;
    std::optional<RenderMetrics> metrics;
    std::vector<Diagnostic> diagnostics;
    // Effective semantic-policy report JSON (UTF-8), populated when the request set
    // RenderOptions::capture_semantic_report. nullopt otherwise. The default member
    // initializer lets the many aggregate error-returns omit this trailing field
    // without tripping -Wmissing-field-initializers.
    std::optional<std::string> semantic_report_json = std::nullopt;

    [[nodiscard]] bool success() const noexcept { return error.ok(); }
};

// Restricts a backend's inline loudness / True-Peak measurement to a frame
// sub-range of the rendered output timeline. Backends still render and write the
// full timeline (so decorrelator / delay state stays continuous); only the meter
// is fed [start_frame, start_frame + frame_count). RenderService sets this when an
// output trim is requested so the reported metrics describe the kept segment.
struct MeterWindow {
    uint64_t start_frame{0};
    uint64_t frame_count{0};
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
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    BinauralSpreadMode binaural_spread_mode{BinauralSpreadMode::automatic};
    // When set, restrict loudness / True-Peak measurement to this output frame
    // window (matches the output trim); nullopt measures the whole render.
    std::optional<MeterWindow> meter_window;
    // Cooperative cancellation token (copied from RenderOptions by RenderService).
    // Backends check cancel_token.stop_requested() at chunk boundaries and return
    // ErrorCode::cancelled when a stop is requested. A default-constructed token
    // never stops.
    std::stop_token cancel_token;
};

// Abstract renderer backend interface.
class IRenderer {
  public:
    virtual ~IRenderer() = default;
    [[nodiscard]] virtual CapabilityReport capabilities() const = 0;
    [[nodiscard]] virtual Result<RenderMetrics>
    render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) = 0;
};

// File-level ADM scene summary returned by RenderService::probe().
// Contains only the top-level counts and audio info; does not expose per-object
// or per-block detail (use mradm::io::import_scene for that directly).
struct SceneProbe {
    uint32_t sample_rate{0};
    uint16_t num_channels{0};
    uint64_t num_frames{0};
    uint32_t programme_count{0};
    uint32_t object_count{0};
};

// One row of the output channel-order reference (mirrors `mradm layouts`):
// for a given container format + layout, the channel count, container mapping
// description, final channel order, an optional note, and which renderer
// backends support the layout.
struct OutputLayoutRow {
    std::string format;                    // "wav" / "caf" / "flac" / "apac" / "iamf"
    std::string layout;                    // "7.1.4" / "binaural" / "hoa3" / ...
    uint32_t channels{0};                  // total output channels (including LFE)
    std::string container;                 // container mapping description
    std::string order;                     // final channel order
    std::string note;                      // human-readable caveat; empty when none
    std::vector<std::string> supported_by; // subset of {ear, saf, hoa, binaural}
};

class RenderService {
  public:
    RenderService();

    [[nodiscard]] RenderResult render(const RenderRequest& request, ProgressSink& progress, LogSink& logs) const;

    // Quickly import the ADM scene and return file-level metadata without rendering.
    // Returns io_error if the file is missing or not a valid ADM BWF file.
    [[nodiscard]] Result<SceneProbe> probe(const std::string& input_path) const;

    // Import the full ADM scene and serialize it to a JSON string (UTF-8).
    // The JSON mirrors the `mradm inspect` field set: file info, programmes,
    // contents, objects (with per-track/per-block detail), HOA tracks, and
    // import warnings. Returns io_error if the file is missing or invalid.
    [[nodiscard]] Result<std::string> inspect_json(const std::string& input_path) const;

    // Enumerate the available renderer backends and their capabilities, serialized
    // to a JSON string (UTF-8). Mirrors the `mradm backends` field set: per-backend
    // feature flags and the list of supported output layouts. Pure in-memory query
    // (no I/O), so it does not fail.
    [[nodiscard]] std::string capabilities_json() const;

    // The output channel-order reference table (mirrors `mradm layouts`), with
    // per-row supported_by computed from the renderer capabilities. Pure
    // in-memory query (no I/O), so it does not fail.
    [[nodiscard]] std::vector<OutputLayoutRow> output_layouts() const;

    // The same table serialized to a JSON string (UTF-8) for the C ABI.
    [[nodiscard]] std::string layouts_json() const;

    // Return the raw <axml> chunk (ADM XML) embedded in the BWF file, verbatim.
    // Mirrors `mradm inspect --xml`. Returns io_error if the file is missing,
    // not a valid BWF, or carries no axml chunk.
    [[nodiscard]] Result<std::string> axml(const std::string& input_path) const;

    // Build the editable neutral semantic-policy template for the scene as a JSON
    // string (UTF-8). Mirrors `mradm inspect --write-semantic-policy-template` but
    // returned in-memory. Returns io_error if the file is missing or invalid.
    [[nodiscard]] Result<std::string> policy_template_json(const std::string& input_path) const;
};

} // namespace mradm
