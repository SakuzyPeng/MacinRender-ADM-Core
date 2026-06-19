#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "adm/errors.h"
#include "adm/live_override.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/scene.h"

#include "audio_output_device.h"
#include "render_stream_factory.h"
#include "ring_buffer.h"

// Realtime monitor engine: a worker thread renders an IRenderStream ahead of the playhead
// into an SPSC ring; the injected IAudioOutputDevice's pull callback drains the ring at
// device rate. Decoupling the (allocating, thread-pooled) DSP from the audio callback via
// the ring is what lets the batch backends run in realtime without becoming lock-free.
// See docs/architecture/REALTIME_MONITORING.md §4 / REALTIME_MONITORING_SLICE1.md §4.
namespace mradm::realtime {

// Internal state; the C ABI maps it to its own int32_t status field.
enum class MonitorState : uint8_t { stopped = 0, playing = 1, paused = 2 };

// Polled status snapshot (no callbacks into managed code; see REALTIME_MONITORING.md §11).
struct MonitorStatus {
    MonitorState state{MonitorState::stopped};
    uint64_t playhead_frames{0}; // frames actually played out (consumer side)
    uint64_t underruns{0};
    uint64_t buffered_frames{0};   // frames currently rendered-ahead in the ring
    float ring_fill{0.0F};         // 0..1 occupancy of the ring
    bool ended{false};             // stream reached end-of-material (no more to render)
    bool failed{false};            // a stream render error stopped production
    uint64_t override_revision{0}; // revision of the last live overrides the worker applied
};

inline constexpr std::size_t k_max_level_channels = 64U;

// Per-channel peak / RMS of the most recently played block.
struct MonitorLevels {
    uint32_t channels{0};
    std::array<float, k_max_level_channels> peak{};
    std::array<float, k_max_level_channels> rms{};
};

class MonitorEngine {
  public:
    // Open a stream via `factory` for (scene, opts) and start `device` pulling from it.
    // `device` and `factory` must outlive the engine. Returns the backend's error (e.g.
    // ErrorCode::unsupported when the resolved backend has no realtime stream).
    [[nodiscard]] static Result<std::unique_ptr<MonitorEngine>> create(IRenderStreamFactory& factory,
                                                                       IAudioOutputDevice& device,
                                                                       const AdmScene& scene,
                                                                       const RenderOptions& opts,
                                                                       LogSink& logs);

    ~MonitorEngine();
    MonitorEngine(const MonitorEngine&) = delete;
    MonitorEngine& operator=(const MonitorEngine&) = delete;
    MonitorEngine(MonitorEngine&&) = delete;
    MonitorEngine& operator=(MonitorEngine&&) = delete;

    void play();
    void pause();
    // Jump the playhead to `frame`: flushes buffered audio so the new position is heard
    // promptly (bounded handshake with the audio thread; see seek() impl).
    void seek(uint64_t frame);
    // Loop output frames [start_frame, end_frame). end_frame <= start_frame disables looping.
    void set_loop(uint64_t start_frame, uint64_t end_frame);
    // Queue live per-object overrides; the worker hands them to the stream at the next
    // block boundary (gain immediate). The applied revision shows up in status().
    void set_overrides(const LiveOverrides& overrides);

    // Hot-swap the rendering stream (e.g. a different backend / same layout) with a short
    // linear crossfade. `next` MUST report the same out_channels() and sample_rate() as the
    // current stream (the caller validates this). The worker seeks `next` to the current
    // playhead and crossfades; takes effect after the ring drains, like the other edits.
    void switch_stream(std::unique_ptr<IRenderStream> next);

    [[nodiscard]] MonitorStatus status() const;
    [[nodiscard]] MonitorLevels levels() const;

    // Fixed monitor output format (set at creation). Used to validate a hot-switch.
    [[nodiscard]] uint32_t out_channels() const { return channels_; }
    [[nodiscard]] uint32_t sample_rate() const { return sample_rate_; }

  private:
    MonitorEngine(std::unique_ptr<IRenderStream> stream, IAudioOutputDevice& device, LogSink& logs);

    void worker_loop();
    bool top_up_ring();                                         // producer side; returns true if it produced
    std::size_t pull(std::span<float> out, std::size_t frames); // consumer side (audio thread)
    void apply_seek_locked(uint64_t frame);                     // worker/control side, under control_mutex_
    void finalize_crossfade();                                  // worker side: snap to the incoming stream

    std::unique_ptr<IRenderStream> stream_;
    IAudioOutputDevice& device_;
    LogSink& logs_; // runtime diagnostics (worker render/seek errors); must outlive the engine
    uint32_t channels_{0};
    uint32_t sample_rate_{0};

    FloatRingBuffer ring_;
    std::vector<float> scratch_;   // worker production scratch (one canonical chunk)
    std::vector<float> scratch_b_; // second chunk, for the incoming stream during a crossfade

    std::thread worker_;
    std::mutex control_mutex_; // serialises play/pause/seek/set_loop + worker command apply
    std::condition_variable wake_;

    std::atomic<MonitorState> state_{MonitorState::stopped};
    std::atomic<bool> quit_{false};
    std::atomic<bool> in_pop_{false};        // audio thread is inside the ring-touching path
    std::atomic<bool> flushing_{false};      // seek in progress: audio thread must not touch the ring
    std::atomic<bool> ended_{false};         // producer hit end-of-material; stop calling process()
    std::atomic<bool> failed_{false};        // producer hit a render error; stop calling process()
    std::atomic<uint64_t> frames_played_{0}; // consumer playhead

    std::atomic<uint64_t> underruns_{0};

    // Loop bounds: written by the control thread (set_loop), read by the worker. 0 = no loop.
    std::atomic<uint64_t> loop_start_{0};
    std::atomic<uint64_t> loop_end_{0};
    // Producer-only (worker thread): current stream output position.
    uint64_t producer_pos_{0};

    // Pending user seek, applied by the worker. Guarded by control_mutex_.
    bool seek_pending_{false};
    uint64_t seek_target_{0};

    // Pending live overrides, applied by the worker via stream_->set_overrides(). Guarded
    // by control_mutex_. applied_override_revision_ is published for status() polling.
    bool overrides_pending_{false};
    LiveOverrides pending_overrides_;
    std::atomic<uint64_t> applied_override_revision_{0};
    // Worker-owned copy of the last applied overrides, re-applied to an incoming stream on a
    // hot-switch so a switched backend keeps the user's edits (status already shows them).
    LiveOverrides current_overrides_;

    // Pending backend hot-switch (control thread → worker), guarded by control_mutex_.
    bool switch_pending_{false};
    std::unique_ptr<IRenderStream> pending_stream_;
    // Crossfade state, worker-owned: while active, top_up_ring renders both stream_ (old)
    // and xfade_stream_ (incoming) and linearly blends across k_crossfade_frames.
    std::unique_ptr<IRenderStream> xfade_stream_;
    bool xfade_active_{false};
    uint64_t xfade_pos_{0};

    // Levels, written by the audio thread, read by status pollers.
    std::array<std::atomic<float>, k_max_level_channels> peak_{};
    std::array<std::atomic<float>, k_max_level_channels> rms_{};
};

} // namespace mradm::realtime
