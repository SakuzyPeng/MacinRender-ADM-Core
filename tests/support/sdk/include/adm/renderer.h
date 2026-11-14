#pragma once

// The render results and C++ backend interfaces the test renderers, the file streams and the SDK's RenderService
// drive. Production calls the Apple backend directly (src/adm_apple/apple_backend.h).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "adm/capability.h"
#include "adm/errors.h"
#include "adm/live_override.h"
#include "adm/logging.h"
#include "adm/progress.h"
#include "adm/render.h"
#include "adm/room.h"

namespace mradm {

struct Diagnostic {
    LogLevel level{LogLevel::info};
    std::string message;
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

// A policy-applied Rust render scene, imported once for many renders.
struct PreparedScene {
    std::shared_ptr<const detail::RenderSceneHandle> handle;
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

// A persistent, stateful file streaming render session for realtime monitoring. Where
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
    // A hint for preparing an incoming backend while the outgoing stream keeps playing.
    [[nodiscard]] virtual bool seek_requires_preroll() const { return false; }

    // Long state restoration stays on a worker and cooperatively yields to a newer request.
    [[nodiscard]] virtual Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) {
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "stream seek cancelled");
        }
        return seek(frame);
    }

    // Read-only validation against immutable prepared metadata. May run on the control thread
    // concurrently with process(); implementations must not inspect mutable DSP state.
    [[nodiscard]] virtual Result<void> validate_overrides(const LiveOverrides& overrides) const {
        (void) overrides;
        return {};
    }

    // Apply live per-object targets. Called on the worker thread at block boundaries. Implemented
    // streams de-zipper gain in the sample domain; SAF binaural coalesces topology targets and
    // crossfades the outgoing/incoming source graphs. The default keeps the prepared values.
    virtual void set_overrides(const LiveOverrides& overrides) { (void) overrides; }

    // Apply a live listener head orientation (yaw/pitch/roll). Called on the worker thread at
    // block boundaries (never the audio callback), e.g. driven by a head-tracking sensor or a
    // manual free-look control. The Apple AUSpatialMixer binaural backend (HeadYaw/Pitch/Roll live
    // AudioUnit params, no re-prepare) and the SAF binaural backend (per-source HRTF direction
    // rotation) implement it; other backends ignore it. NOT pure virtual so backends adopt it
    // incrementally; the default keeps the prepared (identity) orientation.
    virtual void set_listener_orientation(const ListenerOrientation& orientation) { (void) orientation; }

    [[nodiscard]] virtual std::shared_ptr<IRoomControl> room_control() const { return {}; }
    // Room paths append their finite acoustic support to complete programmes.
    [[nodiscard]] virtual std::uint64_t appended_tail_frames() const { return 0; }

    // ── Output-stage orientation (two-stage rendering) ──────────────────────────────────────
    // Opt-in contract for streams that can apply the listener head orientation at the audio
    // OUTPUT (device callback) stage instead of baking it into the worker's render-ahead. When a
    // stream's render is decoupled from the head pose, the pose only enters at the very last step,
    // so head tracking is heard at playout with near-zero latency (no ring-drain delay) — matching
    // the system spatializer. Rendering is split in two:
    //
    //   Stage A  produce_intermediate()  — worker thread, orientation-INDEPENDENT, buffered ahead.
    //   Stage B  render_output()         — device audio callback, applies the CURRENT orientation.
    //
    // The monitor engine buffers Stage A output (intermediate_channels() wide) in its ring and runs
    // Stage B in the pull callback with the latest orientation. Default: not supported — the engine
    // uses the single-stage process() path (orientation applied at render-ahead time). Only the
    // Apple AUSpatialMixer binaural backend opts in; the contract is backend-agnostic so other
    // backends (e.g. a future HOA→binaural that rotates the soundfield at output) can adopt it.
    [[nodiscard]] virtual bool renders_orientation_at_output() const { return false; }

    // Width (channel count) of the intermediate representation produce_intermediate() emits and
    // render_output() consumes. Apple: the source PCM channel count. 0 when not supported.
    [[nodiscard]] virtual uint32_t intermediate_channels() const { return 0; }

    // Stage A: produce `frames` of the orientation-INDEPENDENT intermediate representation into
    // `out` (>= frames * intermediate_channels() floats). Returns frames produced (fewer at
    // end-of-material). Worker thread, like process() — may allocate / read files / use a pool.
    [[nodiscard]] virtual Result<std::size_t> produce_intermediate(std::span<float> out, std::size_t frames) {
        (void) out;
        (void) frames;
        return std::size_t{0};
    }

    // Stage B: render `frames` of final interleaved output (>= frames * out_channels()) from
    // `intermediate` (frames * intermediate_channels()), applying listener orientation `o`. Returns
    // frames produced. Runs on the HARD-REALTIME audio callback: it MUST NOT allocate, lock, block,
    // or do I/O — preallocate all scratch and read the orientation from the passed value.
    [[nodiscard]] virtual std::size_t render_output(std::span<const float> intermediate,
                                                    std::span<float> out,
                                                    std::size_t frames,
                                                    const ListenerOrientation& o) {
        (void) intermediate;
        (void) out;
        (void) frames;
        (void) o;
        return 0;
    }

    // Output-stage only: true once render_output() hit a fatal error on the audio callback (it
    // silenced the affected block rather than emitting garbage). The engine polls this off the
    // callback to stop / report the monitor. Default false (single-stage backends report render
    // errors through process()'s Result instead).
    [[nodiscard]] virtual bool render_failed() const { return false; }

    // Output-stage loop support. Producer (Stage A) and consumer (Stage B) wrap independently:
    //  - reposition_source(): worker thread — reposition ONLY the source-read position for a loop
    //    wrap, leaving the render cursors / AU state untouched (the consumer is still playing earlier
    //    frames). Default forwards to seek() — fine for single-stage where the producer IS the render.
    //  - loop_render_reset(): audio callback — wrap the render cursors (metadata/event cursors) to
    //    `frame` when playout reaches the loop boundary. Realtime-safe (no AU reset / alloc / lock).
    //    Default no-op.
    [[nodiscard]] virtual Result<void> reposition_source(uint64_t frame) { return seek(frame); }
    virtual void loop_render_reset(uint64_t frame) { (void) frame; }

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
    // meter_window / cancel_token. Only the Apple backend implements it: the Rust backends'
    // offline windows render in the engine.
    [[nodiscard]] virtual Result<RenderMetrics>
    render_window(const IPreparedRender& prepared, const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
        (void) prepared;
        (void) plan;
        (void) progress;
        (void) logs;
        return make_error(ErrorCode::unsupported, "this backend renders offline windows in the render engine");
    }

    // Convenience: prepare + render_window in one call (the non-preview path).
    [[nodiscard]] Result<RenderMetrics> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
        auto prepared = prepare(plan, logs);
        if (!prepared) {
            return mradm::unexpected{prepared.error()};
        }
        return render_window(**prepared, plan, progress, logs);
    }

    // Open a persistent file streaming session (realtime monitoring) reusing `prepared` —
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

} // namespace mradm
