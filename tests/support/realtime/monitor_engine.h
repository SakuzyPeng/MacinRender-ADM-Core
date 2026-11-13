#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "adm/errors.h"
#include "adm/live_override.h"
#include "adm/logging.h"
#include "adm/options.h"
#include "adm/scene.h"

#include "audio_output_device.h"
#include "engine_test_ffi.h"
#include "hptf_eq.h"
#include "render_stream_factory.h"

// File streaming monitor engine: a worker renders ahead of the playhead into a ring that the
// device callback drains (docs/architecture/REALTIME_MONITORING.md §4).
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

// Per-channel peak / RMS of the most recently played block, plus program loudness (LUFS,
// ITU-R BS.1770) of the monitored output. LUFS is -inf when below the gate / silent.
struct MonitorLevels {
    uint32_t channels{0};
    std::array<float, k_max_level_channels> peak{};
    std::array<float, k_max_level_channels> rms{};
    float momentary_lufs{-std::numeric_limits<float>::infinity()};  // 400 ms window
    float shortterm_lufs{-std::numeric_limits<float>::infinity()};  // 3 s window
    float integrated_lufs{-std::numeric_limits<float>::infinity()}; // gated, since the last seek
};

enum class StreamSwitchState : uint8_t { pending, applied, rejected };

struct StreamSwitchReceipt {
    std::atomic<StreamSwitchState> state{StreamSwitchState::pending};
};

// The realtime monitor (mradm-engine `monitor`, PCM consistency 5.5 A): the worker, the ring, seeks,
// loops, overrides, orientation, backend switches and crossfades, HpTF, the LUFS meter and the
// levels run in Rust. This shell keeps the device: its callback forwards to the Rust pull, and the
// engine flushes, pauses and resumes it through host callbacks. Rust file streams are handed over
// directly; other streams (the Apple island, test streams) through a C callback table.
class MonitorEngine {
  public:
    // room_control adopts a Rust room stream's control handle for room_control().
    [[nodiscard]] static Result<std::unique_ptr<MonitorEngine>>
    create(IRenderStreamFactory& factory,
           IAudioOutputDevice& device,
           const AdmScene& scene,
           const RenderOptions& opts,
           LogSink& logs,
           std::shared_ptr<IRoomControl> (*room_control)(MradmRenderRoomControl*) = nullptr);

    ~MonitorEngine();
    MonitorEngine(const MonitorEngine&) = delete;
    MonitorEngine& operator=(const MonitorEngine&) = delete;
    MonitorEngine(MonitorEngine&&) = delete;
    MonitorEngine& operator=(MonitorEngine&&) = delete;

    void play();
    void pause();
    void seek(uint64_t frame);
    void set_loop(uint64_t start_frame, uint64_t end_frame);
    Result<void> set_overrides(const LiveOverrides& overrides);

    void set_listener_orientation(const ListenerOrientation& orientation);
    [[nodiscard]] std::shared_ptr<IRoomControl> room_control() const;

    void switch_stream(std::unique_ptr<IRenderStream> next, std::shared_ptr<StreamSwitchReceipt> receipt = {});

    // Headphone compensation (HpTF) on the final output, published lock-free to the callback.
    // Only meaningful for a 2-channel (headphone) monitor feed — see hptf_supported().
    void set_hptf(const std::optional<render_common::HptfCoefficients>& coeffs, uint64_t revision);
    void set_hptf(const render_common::HptfSnapshot& snapshot) { hptf_.publish(snapshot); }

    [[nodiscard]] bool hptf_supported() const { return channels_ == 2U; }
    [[nodiscard]] uint64_t hptf_applied_revision() const { return hptf_.applied_revision(); }
    [[nodiscard]] render_common::HptfCoefficients hptf_active_coefficients() const {
        return hptf_.active_coefficients();
    }
    [[nodiscard]] render_common::HptfSnapshot hptf_active_snapshot() const { return hptf_.active_snapshot(); }

    [[nodiscard]] MonitorStatus status() const;
    [[nodiscard]] MonitorLevels levels() const;

    // Fixed monitor output format (set at creation). Used to validate a hot-switch.
    [[nodiscard]] uint32_t out_channels() const { return channels_; }
    [[nodiscard]] uint32_t sample_rate() const { return sample_rate_; }
    // True when the stream renders the listener orientation at the output (callback) stage.
    [[nodiscard]] bool output_stage() const { return output_stage_; }

  private:
    struct Destroy {
        void operator()(MradmEngineLiveMonitor* handle) const noexcept;
    };

    MonitorEngine(IAudioOutputDevice& device, LogSink& logs);

    IAudioOutputDevice& device_;
    LogSink& logs_; // runtime diagnostics; must outlive the engine
    std::shared_ptr<IRoomControl> (*room_control_)(MradmRenderRoomControl*){nullptr};
    MradmEngineLiveMonitorHost host_{};
    std::unique_ptr<MradmEngineLiveMonitor, Destroy> rust_;
    render_common::HptfProcessor hptf_; // a view of the Rust monitor's processor
    uint32_t channels_{0};
    uint32_t sample_rate_{0};
    bool output_stage_{false};
};

} // namespace mradm::realtime
