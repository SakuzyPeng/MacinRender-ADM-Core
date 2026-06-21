#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "adm/errors.h"
#include "adm/live_override.h"
#include "adm/logging.h"
#include "adm/options.h"

namespace mradm {

// Realtime monitor playback state (mirrors the engine's internal state).
enum class MonitorPlaybackState : std::uint8_t { stopped = 0, playing = 1, paused = 2 };

inline constexpr std::size_t k_monitor_max_level_channels = 64U;

// Polled status snapshot (no callbacks; see docs/architecture/REALTIME_MONITORING.md §11).
struct MonitorStatusSnapshot {
    MonitorPlaybackState state{MonitorPlaybackState::stopped};
    std::uint64_t playhead_frames{0}; // frames actually played out
    std::uint64_t underruns{0};
    std::uint64_t buffered_frames{0};   // frames rendered ahead of the playhead
    float ring_fill{0.0F};              // 0..1 ring occupancy
    bool ended{false};                  // stream reached end of material
    bool failed{false};                 // a render error stopped production
    std::uint64_t override_revision{0}; // revision of the last applied live overrides
};

// Per-channel peak / RMS of the most recently played block, plus program loudness (LUFS,
// ITU-R BS.1770) of the monitored output. LUFS is -inf when below the gate / silent.
struct MonitorLevelsSnapshot {
    std::uint32_t channels{0};
    std::array<float, k_monitor_max_level_channels> peak{};
    std::array<float, k_monitor_max_level_channels> rms{};
    float momentary_lufs{-std::numeric_limits<float>::infinity()};  // 400 ms window
    float shortterm_lufs{-std::numeric_limits<float>::infinity()};  // 3 s window
    float integrated_lufs{-std::numeric_limits<float>::infinity()}; // gated, since the last seek
};

// A persistent realtime monitor over an ADM file: it streams the rendered scene to the
// default audio output device while play / pause / seek / loop control playback, and
// status / levels / diagnostics are polled. The heavy realtime machinery (streaming render
// session, SPSC ring, worker thread, audio device) is hidden behind a pimpl so this public
// header carries no third-party / backend types (ADR 0003). Declared here (ADMCore-level
// header) but implemented in ADMEngine, like RenderService.
class MonitorSession {
  public:
    // Import `input_path`, resolve the backend + apply the semantic policy from `options`,
    // and start monitoring through the default audio output device. Returns the import /
    // policy / backend error, ErrorCode::unsupported when the resolved backend has no
    // realtime stream, or an internal error when no audio output device is available.
    [[nodiscard]] static Result<std::unique_ptr<MonitorSession>> create(const std::string& input_path,
                                                                        const RenderOptions& options);

    ~MonitorSession();
    MonitorSession(const MonitorSession&) = delete;
    MonitorSession& operator=(const MonitorSession&) = delete;
    MonitorSession(MonitorSession&&) = delete;
    MonitorSession& operator=(MonitorSession&&) = delete;

    void play();
    void pause();
    // Jump the playhead. Negative clamps to 0; past the end stops at the end.
    [[nodiscard]] Result<void> seek_seconds(double seconds);
    // Loop [start, end). end <= start disables looping.
    void set_loop_seconds(double start_seconds, double end_seconds);
    // Apply live per-object overrides. gain is immediate (next block); the
    // diffuse/extent/divergence scales take effect on the binaural backend via a cheap
    // re-prepare, and are accepted-but-ignored by backends not yet wired up (e.g. Apple,
    // gain only). The applied revision is reported via status().override_revision.
    void set_overrides(const LiveOverrides& overrides);

    // Hot-switch the rendering backend / layout live (e.g. EAR↔VBAP↔Apple at the same
    // layout, or binaural↔Apple-binaural), reusing the already-imported + policy-applied
    // scene. The new backend is prepared off the audio thread, then crossfaded in. The new
    // stream must run at the current monitor sample rate. A different channel count is folded
    // into the fixed monitor output when the monitor is stereo (speaker layouts by geometry,
    // HOA by a first-order decode); other channel-count changes return
    // ErrorCode::unsupported. Returns the backend resolve / prepare error on failure.
    [[nodiscard]] Result<void> switch_backend(const RenderOptions& options);

    [[nodiscard]] MonitorStatusSnapshot status() const;
    [[nodiscard]] MonitorLevelsSnapshot levels() const;

    // Polled diagnostics buffer (backend resolution / import warnings, errors), append-only
    // for the session's lifetime: snapshot log_count(), then read [0, count).
    [[nodiscard]] std::size_t log_count() const;
    [[nodiscard]] bool log_entry(std::size_t index, LogLevel& level, std::string& module, std::string& message) const;

  private:
    MonitorSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mradm
