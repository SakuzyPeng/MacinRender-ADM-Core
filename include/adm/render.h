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

// Requests that a backend render ONLY the output frame window
// [start_frame, start_frame + frame_count) instead of the full timeline. A backend
// that advertises CapabilityReport::supports_render_window seeks its input and runs
// an internal warm-up pre-roll (enough to converge any finite-memory DSP state:
// decorrelator overlap, compensation delay, etc.), then writes exactly the window —
// the output file is already trimmed and the inline meter measures only the window,
// bit-identical to a full render followed by trim_file_frames. Backends that do not
// support it ignore this field; RenderService then renders the full timeline and
// trims afterward. Frames are on the output timeline, which equals the input
// timeline. Set by RenderService in place of meter_window when the chosen backend
// supports it.
struct RenderWindow {
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
    // When set, render only this output sub-window with internal pre-roll instead of
    // the full timeline (backends advertising supports_render_window). Mutually
    // exclusive with meter_window: when render_window is set the backend meters
    // exactly the frames it writes (the window).
    std::optional<RenderWindow> render_window;
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

    // Render a request. When preimported_scene is non-null it is used directly
    // (copied) instead of importing from disk, and semantic-policy application plus
    // the semantic report are skipped — the caller (PreviewSession) is responsible
    // for having imported and applied the policy already. When null, the full path
    // runs: import, semantic policy, optional report.
    [[nodiscard]] RenderResult render(const RenderRequest& request,
                                      ProgressSink& progress,
                                      LogSink& logs,
                                      const AdmScene* preimported_scene = nullptr) const;

    // Import the ADM scene and apply the semantic policy from `options` (in-memory
    // JSON preferred over file path), returning the policy-applied scene. Used by
    // PreviewSession to do the expensive import + policy once, then reuse the scene
    // across many window renders. Returns the import / policy error on failure.
    [[nodiscard]] Result<AdmScene>
    prepare_preview_scene(const std::filesystem::path& input_path, const RenderOptions& options, LogSink& logs) const;

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

    // The output container-format reference serialized to a JSON string (UTF-8):
    // per-format availability (build/platform) and constraints, plus a "features"
    // object of build flags. Mirrors no CLI command today; drives a GUI's format
    // picker. Does no project-file I/O and does not fail, but in IAMF-enabled builds
    // the "iamf_mp4_packager" flag probes PATH and may spawn a short-lived
    // mp4box/ffmpeg subprocess (no probe in the default MR_ADM_ENABLE_IAMF=OFF build).
    [[nodiscard]] std::string output_formats_json() const;

    // Return the raw <axml> chunk (ADM XML) embedded in the BWF file, verbatim.
    // Mirrors `mradm inspect --xml`. Returns io_error if the file is missing,
    // not a valid BWF, or carries no axml chunk.
    [[nodiscard]] Result<std::string> axml(const std::string& input_path) const;

    // Build the editable neutral semantic-policy template for the scene as a JSON
    // string (UTF-8). Mirrors `mradm inspect --write-semantic-policy-template` but
    // returned in-memory. Returns io_error if the file is missing or invalid.
    [[nodiscard]] Result<std::string> policy_template_json(const std::string& input_path) const;
};

// A reusable preview / scrubbing session. Imports the ADM scene and applies the
// semantic policy ONCE at creation, then renders arbitrary output sub-windows of the
// same (input, options) cheaply by reusing the cached scene (skipping re-import +
// policy) together with on-demand window rendering. Intended for GUI timeline
// scrubbing. Not thread-safe; use one session per thread or serialize access.
// (Phase 2a: the scene is cached; per-window backend state — gain matrices / HRTF —
// is still rebuilt each call, which Phase 2b will cache.)
class PreviewSession {
  public:
    // Import + apply the semantic policy from `options`. The output time-range trim
    // fields (render_start_sec / render_end_sec) in `options` are ignored here — the
    // window is supplied per call to render_window. Returns the import / policy error.
    [[nodiscard]] static Result<PreviewSession>
    create(std::filesystem::path input_path, RenderOptions options, LogSink& logs);

    // Render the output window [start_sec, end_sec) of the cached scene to output_path.
    // nullopt start = from the beginning; nullopt end = to the end. Reuses the cached
    // scene; the chosen backend renders only the window (on-demand) when it supports it.
    [[nodiscard]] RenderResult render_window(std::optional<double> start_sec,
                                             std::optional<double> end_sec,
                                             std::optional<std::filesystem::path> output_path,
                                             ProgressSink& progress,
                                             LogSink& logs) const;

    [[nodiscard]] const AdmScene& scene() const noexcept { return scene_; }

  private:
    PreviewSession(std::filesystem::path input, RenderOptions options, AdmScene scene);

    std::filesystem::path input_;
    RenderOptions options_;
    AdmScene scene_;
    RenderService service_;
};

} // namespace mradm
