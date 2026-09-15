#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "adm/errors.h"
#include "adm/options.h"

#include "../adm_render_common/hptf_eq.h"
#include "audio_output_device.h"
#include "scene_stream_engine.h"
#include "stereo_peak_guard.h"

namespace mradm::realtime {

// Cross-translation-unit aggregates consumed by the C ABI and implementation.
// cppcheck-suppress-begin unusedStructMember

enum class SceneOutputKind { system_spatial, stereo, null_device };
enum class SceneOutputState { paused, playing, buffering, draining, ended, failed };

struct SceneDeviceConfig {
    SceneOutputKind kind{SceneOutputKind::system_spatial};
    std::string layout;
    std::string device_id;
    SpeakerGeometry geometry{SpeakerGeometry::standard};
};

struct SceneDeviceStatus {
    SceneOutputState state{SceneOutputState::paused};
    std::uint64_t epoch_id{0};
    std::uint64_t consumed_frames{0};
    std::uint64_t presented_frames{0};
    std::uint64_t queued_frames{0};
    std::uint64_t underruns{0};
    bool media_clock{false};
    bool recovering{false};
    bool failed{false};
    bool ended{false};
};

class SceneOutputSession {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SceneOutputSession>>
    create(const std::shared_ptr<SceneStreamEngine>& stream, const SceneDeviceConfig& config);
    // Internal injection boundary, also used by deterministic device tests.
    [[nodiscard]] static Result<std::unique_ptr<SceneOutputSession>>
    create_with_device(const std::shared_ptr<SceneStreamEngine>& stream,
                       std::unique_ptr<IAudioOutputDevice> device,
                       bool protect_stereo);
    ~SceneOutputSession();
    SceneOutputSession(const SceneOutputSession&) = delete;
    SceneOutputSession& operator=(const SceneOutputSession&) = delete;

    void play();
    void pause();
    [[nodiscard]] Result<void> begin_epoch(std::uint64_t epoch, std::int64_t target);
    [[nodiscard]] Result<void> set_volume(float gain);

    // Publish a headphone-compensation (HpTF) cascade, or bypass when `coeffs` is nullopt.
    // Takes control_ like set_volume and deliberately does NOT park(): parking the device on every
    // profile change would stall playback, and the handoff to the callback is lock-free anyway.
    // Returns `unsupported` unless this session owns the stereo headphone feed — a multichannel
    // bed handed to the OS for HRTF has no 2ch signal on our side to compensate.
    [[nodiscard]] Result<void> set_hptf(const std::optional<render_common::HptfCoefficients>& coeffs,
                                        std::uint64_t revision);
    [[nodiscard]] bool hptf_supported() const { return peak_guard_ != nullptr; }
    [[nodiscard]] std::uint64_t hptf_applied_revision() const { return hptf_.applied_revision(); }
    [[nodiscard]] render_common::HptfCoefficients hptf_active_coefficients() const {
        return hptf_.active_coefficients();
    }

    [[nodiscard]] SceneDeviceStatus status() const;

  private:
    SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream,
                       std::unique_ptr<IAudioOutputDevice> device,
                       bool protect_stereo);
    void park();
    std::size_t pull(std::span<float> output, std::size_t frames) noexcept;
    [[nodiscard]] ScenePullResult pull_stereo(std::span<float> output, std::uint32_t frames) noexcept;

    std::shared_ptr<SceneStreamEngine> stream_;
    std::unique_ptr<IAudioOutputDevice> device_;
    mutable std::mutex control_;
    std::atomic<bool> playing_{false};
    std::atomic<std::uint32_t> pulls_{0};
    std::atomic<float> volume_{1.0F};
    std::atomic<std::uint64_t> consumed_{0};
    std::atomic<std::uint64_t> presented_{0};
    std::atomic<bool> eos_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> buffering_{true};
    std::uint32_t channels_{0};
    std::unique_ptr<StereoPeakGuard> peak_guard_;
    std::vector<float> peak_input_;
    bool peak_source_ended_{false};
    // Headphone compensation, applied to peak_input_ upstream of peak_guard_. Only prepared when
    // this session owns a stereo feed; otherwise it stays a no-op bypass.
    render_common::HptfProcessor hptf_;
};

// cppcheck-suppress-end unusedStructMember
} // namespace mradm::realtime
