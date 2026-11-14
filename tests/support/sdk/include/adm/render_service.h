// The C++ render service, preview session and their discovery records (test SDK since 6.5 C;
// production uses the Rust C ABI and CLI).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adm/render.h"
#include "adm/renderer.h"

namespace mradm {

// File-level ADM scene summary returned by RenderService::probe().
// Contains only the top-level counts and audio info; does not expose per-object
// or per-block detail (see inspect_json / inspect_text).
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
    std::vector<std::string> supported_by; // subset of {ear, saf, hoa, saf-binaural}
};

// One output container format (mirrors `mradm formats` / adm_output_formats_json):
// its file extensions, availability in this build/platform, and constraints.
struct OutputFormatInfo {
    std::string format;                  // "wav" / "caf" / "flac" / "opus_mka" / "apac" / "iamf" / "iamf_mp4"
    std::vector<std::string> extensions; // e.g. {".m4a", ".mp4"}
    bool available{true};
    std::string available_reason; // human-readable; empty when available
    bool lossy{false};
    uint32_t max_channels{0};      // 0 = unlimited
    uint32_t fixed_sample_rate{0}; // 0 = any
    bool supports_height{false};
    std::vector<std::string> bit_depths; // e.g. {"f32","i24","i16"}; empty when N/A
    bool has_bitrate{false};             // true when a bitrate range applies
    bool bitrate_per_channel{false};     // true = per channel (Opus), false = total (APAC)
    uint32_t bitrate_min_kbps{0};
    uint32_t bitrate_max_kbps{0};
    std::string note; // human-readable caveat; empty when none
};

// Build / platform feature flags (mirrors the "features" object in the JSON).
struct OutputFormatFeatures {
    bool apac{false};
    bool iamf{false};
    bool iamf_mp4_packager{false};
    bool sofa{false};
    // HpTF 耳机补偿（AutoEq ParametricEQ）。无构建/平台门控，本核心恒为 true——它的作用是让
    // GUI 能区分"核心支持但没开"与"核心太旧根本没有这个字段"（缺失即 false）。
    bool hptf{false};
};

// The full output-format reference: per-build feature flags + the container list.
struct OutputFormats {
    OutputFormatFeatures features;
    std::vector<OutputFormatInfo> formats;
};

class RenderService {
  public:
    RenderService();

    // Render a request. When preimported_scene is non-null it is used directly
    // (copied) instead of importing from disk, and semantic-policy application plus
    // the semantic report are skipped — the caller (PreviewSession) is responsible
    // for having imported and applied the policy already. When null, the full path
    // runs: import, semantic policy, optional report.
    //
    // prepared_cache: optional slot holding the chosen backend's prepared state
    // (gain matrices / HRTF). If non-null and empty, it is filled on this call and may
    // be reused on subsequent calls with the same renderer / layout / options (a
    // PreviewSession passes the same slot for every window). If null, prepared state is
    // built per call. The caller must only reuse a slot across calls whose renderer
    // selection and prepare-relevant options are unchanged.
    [[nodiscard]] RenderResult render(const RenderRequest& request,
                                      ProgressSink& progress,
                                      LogSink& logs,
                                      const PreparedScene* preimported_scene = nullptr,
                                      std::shared_ptr<IPreparedRender>* prepared_cache = nullptr) const;

    // Import the ADM scene and apply the semantic policy from `options` (in-memory
    // JSON preferred over file path), returning the policy-applied scene. Used by
    // PreviewSession to do the expensive import + policy once, then reuse the scene
    // across many window renders. Returns the import / policy error on failure.
    [[nodiscard]] Result<PreparedScene>
    prepare_preview_scene(const std::filesystem::path& input_path, const RenderOptions& options, LogSink& logs) const;

    // Quickly import the ADM scene and return file-level metadata without rendering.
    // Returns io_error if the file is missing or not a valid ADM BWF file.
    [[nodiscard]] Result<SceneProbe> probe(const std::string& input_path) const;

    // Import the full ADM scene and serialize it to a JSON string (UTF-8).
    // The JSON mirrors the `mradm inspect` field set: file info, programmes,
    // contents, objects (with per-track/per-block detail), HOA tracks, and
    // import warnings. Returns io_error if the file is missing or invalid.
    [[nodiscard]] Result<std::string> inspect_json(const std::string& input_path) const;
    // The `mradm inspect` text summary (UTF-8, starting with `File: <input_path>`), with the same
    // import as inspect_json. Returns the import error.
    [[nodiscard]] Result<std::string> inspect_text(const std::string& input_path) const;

    // Enumerate the available renderer backends and their capabilities, serialized
    // to a JSON string (UTF-8). Mirrors the `mradm backends` field set: per-backend
    // feature flags and the list of supported output layouts. Pure in-memory query
    // (no I/O), so it does not fail.
    [[nodiscard]] std::string capabilities_json() const;

    // The capability report `mradm backends` prints for a renderer selection: "ear", "saf",
    // "triple-balance", "hoa", "saf-binaural" (or "binaural"), "room-binaural", and "apple" in
    // Apple builds. Nullopt for any other name.
    [[nodiscard]] std::optional<CapabilityReport> backend_capabilities(std::string_view renderer) const;

    // The output channel-order reference table (mirrors `mradm layouts`), with
    // per-row supported_by computed from the renderer capabilities. Pure
    // in-memory query (no I/O), so it does not fail.
    [[nodiscard]] std::vector<OutputLayoutRow> output_layouts() const;

    // The same table serialized to a JSON string (UTF-8) for the C ABI.
    [[nodiscard]] std::string layouts_json() const;

    // Ordinary channel-bed input presets, custom label catalog, aliases, and
    // nominal geometry (schema mradm.input-layouts v1).
    [[nodiscard]] std::string input_layouts_json() const;

    // The output container-format reference (mirrors `mradm formats`): per-build
    // feature flags + the container list with availability and constraints. Single
    // source of truth shared with output_formats_json(). See output_formats_json()
    // for the IAMF packager-probe caveat.
    [[nodiscard]] OutputFormats output_formats() const;

    // The output container-format reference serialized to a JSON string (UTF-8):
    // per-format availability (build/platform) and constraints, plus a "features"
    // object of build flags. Mirrors no CLI command today; drives a GUI's format
    // picker. Does no project-file I/O and does not fail, but in IAMF-enabled builds
    // the "iamf_mp4_packager" flag probes PATH and may spawn a short-lived
    // mp4box/ffmpeg subprocess (no probe in the default MR_ADM_ENABLE_IAMF=OFF build).
    [[nodiscard]] std::string output_formats_json() const;

    // Renderer × layout × output-target support matrix serialized to JSON (UTF-8).
    // This combines capabilities_json(), layouts_json(), and output_formats_json()
    // into concrete supported/reason rows for GUI option pickers.
    [[nodiscard]] std::string render_support_matrix_json() const;

    // Return the raw <axml> chunk (ADM XML) embedded in the BWF file, verbatim.
    // Mirrors `mradm inspect --xml`. Returns io_error if the file is missing,
    // not a valid BWF, or carries no axml chunk.
    [[nodiscard]] Result<std::string> axml(const std::string& input_path) const;

    // Build the editable neutral semantic-policy template for the scene as a JSON
    // string (UTF-8). Mirrors `mradm inspect --write-semantic-policy-template` but
    // returned in-memory. Returns io_error if the file is missing or invalid.
    [[nodiscard]] Result<std::string> policy_template_json(const std::string& input_path) const;

    // Write the template of `input_path` imported with the channel-bed routing of
    // `options` (ADM when the file carries AXML and no input layout or labels are
    // given) to `output_path`, with a trailing newline. Backs
    // `mradm inspect --write-semantic-policy-template`.
    [[nodiscard]] Result<void> write_policy_template(const std::filesystem::path& input_path,
                                                     const std::filesystem::path& output_path,
                                                     const RenderOptions& options) const;

    // Apply the semantic policy from `options` (in-memory JSON preferred over file
    // path) and write a new ADM BWF at output_path, reusing the source PCM and chna
    // chunk verbatim. With no policy in `options` this is a plain ADM round-trip.
    // Mirrors `mradm export`. Returns io_error / unsupported on failure.
    [[nodiscard]] Result<void> export_file(const std::string& input_path,
                                           const std::string& output_path,
                                           const RenderOptions& options,
                                           LogSink& logs) const;
};

// A reusable preview / scrubbing session. Imports the ADM scene and applies the
// semantic policy ONCE at creation, then renders arbitrary output sub-windows of the
// same (input, options) cheaply by reusing the cached scene (skipping re-import +
// policy) together with on-demand window rendering. Intended for GUI timeline
// scrubbing. Not thread-safe; use one session per thread or serialize access.
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

  private:
    PreviewSession(std::filesystem::path input, RenderOptions options, PreparedScene scene);

    std::filesystem::path input_;
    RenderOptions options_;
    PreparedScene scene_;
    RenderService service_;
    // Backend prepared state (gain matrices / HRTF), built lazily on the first
    // render_window and reused for the rest. mutable: render_window is logically const
    // (the session's inputs don't change) but fills this cache on first use.
    mutable std::shared_ptr<IPreparedRender> prepared_;
};

} // namespace mradm
