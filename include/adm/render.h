#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
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
    // 0 disables smoothing. Backend-dependent: the Apple AUSpatialMixer backend
    // currently ignores this option and relies on SpatialMixer's internal smoothing.
    uint32_t object_smoothing_frames{0};
    SpeakerSpreadMode speaker_spread_mode{SpeakerSpreadMode::automatic};
    BinauralSpreadMode binaural_spread_mode{BinauralSpreadMode::automatic};
    AppleSpatialPreset apple_spatial_preset{AppleSpatialPreset::off};
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

// Immutable, reusable backend state produced by IRenderer::prepare() (gain matrices,
// HRTF tables, decoders). Opaque base; each backend returns its own derived type and
// downcasts in render_window(). May be shared across many render_window() calls — e.g.
// a PreviewSession reuses it while scrubbing windows of the same (scene, layout,
// options) — so it must not carry per-output or per-window mutable state.
class IPreparedRender {
  public:
    IPreparedRender() = default;
    virtual ~IPreparedRender() = default;
    IPreparedRender(const IPreparedRender&) = delete;
    IPreparedRender& operator=(const IPreparedRender&) = delete;
    IPreparedRender(IPreparedRender&&) = delete;
    IPreparedRender& operator=(IPreparedRender&&) = delete;
};

// Trivial prepared state for backends that have not yet split their preparation out
// of render_window() (they ignore the prepared argument and rebuild per call).
struct EmptyPreparedRender final : IPreparedRender {};

// A persistent, stateful streaming render session for realtime monitoring. Where
// render_window() renders a window to a file in one batch, an IRenderStream is pulled
// block-by-block: process() advances an internal playhead and carries DSP state
// (convolution overlap, STFT, decorrelator delay) across calls, so consecutive blocks
// join seamlessly. Lives on a worker thread that fills a ring buffer ahead of the audio
// device; it may allocate / use a thread pool (it is NOT the hard-realtime audio
// callback). See docs/architecture/REALTIME_MONITORING.md.
class IRenderStream {
  public:
    virtual ~IRenderStream() = default;
    IRenderStream(const IRenderStream&) = delete;
    IRenderStream& operator=(const IRenderStream&) = delete;
    IRenderStream(IRenderStream&&) = delete;
    IRenderStream& operator=(IRenderStream&&) = delete;

    // Render `frames` frames of interleaved PCM into `out` (out.size() must be
    // >= frames * out_channels()). Returns the number of frames actually produced
    // (fewer than requested at end-of-material or a loop boundary). The caller may
    // request any `frames`; the implementation MUST use its own canonical block size
    // plus a FIFO so the output is bit-identical to the offline render_window path
    // regardless of how the device chunks its callbacks.
    [[nodiscard]] virtual Result<std::size_t> process(std::span<float> out, std::size_t frames) = 0;

    // Seek to an absolute output frame (after editing diffuse/extent/divergence, or a
    // loop-region wrap). Must reset / pre-roll backend state so output after the seek is
    // equivalent to the offline window render at the same position.
    [[nodiscard]] virtual Result<void> seek(uint64_t frame) = 0;

    [[nodiscard]] virtual uint32_t out_channels() const = 0;
    [[nodiscard]] virtual uint32_t sample_rate() const = 0;
    [[nodiscard]] virtual std::string_view output_layout() const = 0;

  protected:
    IRenderStream() = default;
};

// Abstract renderer backend interface.
class IRenderer {
  public:
    virtual ~IRenderer() = default;
    [[nodiscard]] virtual CapabilityReport capabilities() const = 0;

    // Build immutable, reusable state from the scene + layout + options in `plan`.
    // Ignores plan.output_path / render_window / meter_window / cancel_token. The
    // result may be reused across render_window() calls while those inputs are
    // unchanged. Expensive work (gain matrices, HRTF FFT, decoders) belongs here.
    [[nodiscard]] virtual Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) = 0;

    // Render one output (window) using `prepared` — which MUST come from THIS
    // renderer's prepare() — plus the per-call plan.output_path / render_window /
    // meter_window / cancel_token.
    [[nodiscard]] virtual Result<RenderMetrics>
    render_window(const IPreparedRender& prepared, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) = 0;

    // Convenience: prepare + render_window in one call (the non-preview path).
    [[nodiscard]] Result<RenderMetrics> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
        auto prepared = prepare(plan, logs);
        if (!prepared) {
            return tl::unexpected{prepared.error()};
        }
        return render_window(**prepared, plan, progress, logs);
    }

    // Open a persistent streaming session (realtime monitoring) reusing `prepared` —
    // which MUST come from THIS renderer's prepare(). Intentionally NOT pure virtual so
    // backends adopt streaming incrementally; the default reports unsupported. See
    // docs/architecture/REALTIME_MONITORING.md.
    [[nodiscard]] virtual Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prepared, const RenderPlan& plan, LogSink& logs) {
        (void) prepared;
        (void) plan;
        (void) logs;
        return make_error(ErrorCode::unsupported, "backend has no realtime stream", {});
    }
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
                                      const AdmScene* preimported_scene = nullptr,
                                      std::shared_ptr<IPreparedRender>* prepared_cache = nullptr) const;

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
    // Backend prepared state (gain matrices / HRTF), built lazily on the first
    // render_window and reused for the rest. mutable: render_window is logically const
    // (the session's inputs don't change) but fills this cache on first use.
    mutable std::shared_ptr<IPreparedRender> prepared_;
};

} // namespace mradm
