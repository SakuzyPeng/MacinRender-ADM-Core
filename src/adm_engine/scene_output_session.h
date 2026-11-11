#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "adm/errors.h"
#include "adm/monitor.h" // HptfInfo（两条实时路径共用）
#include "adm/options.h"

#include "../adm_render_common/hptf_eq.h"
#include "audio_output_device.h"
#include "scene_stream_engine.h"

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

// The session itself (pull, the stereo feed's prefetch, HpTF and peak guard, volume, epochs,
// status) is Rust mradm-engine `scene::output`; this shell owns the device.
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

    // Path-based form, mirroring MonitorSession::set_hptf: loads an AutoEq ParametricEQ profile,
    // designs it for this stream's rate and publishes it. Empty path disables. Parsing happens on
    // the caller's thread so a bad file is rejected without ever disturbing the audio callback.
    [[nodiscard]] Result<void>
    set_hptf_profile(const std::string& profile_path, HptfPreampMode mode, std::uint64_t revision);

    [[nodiscard]] Result<void> set_hptf_parameters(const HptfProfile& profile,
                                                   HptfPreampMode mode = HptfPreampMode::warn_only,
                                                   std::uint64_t revision = 0);

    [[nodiscard]] HptfInfo hptf_info() const;
    [[nodiscard]] HptfStereoInfo hptf_stereo_info() const;
    [[nodiscard]] Result<void> set_hptf_prepared(const render_common::HptfSettings& settings);
    [[nodiscard]] bool hptf_supported() const { return hptf_supported_; }
    [[nodiscard]] std::uint64_t hptf_applied_revision() const { return hptf_snapshot().revision; }
    [[nodiscard]] render_common::HptfCoefficients hptf_active_coefficients() const {
        return hptf_snapshot().coefficients;
    }

    [[nodiscard]] SceneDeviceStatus status() const;

  private:
    struct Handle;
    SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream,
                       std::unique_ptr<IAudioOutputDevice> device,
                       std::unique_ptr<Handle> handle,
                       bool hptf_supported);
    void park();
    [[nodiscard]] render_common::HptfSnapshot hptf_snapshot() const;

    std::shared_ptr<SceneStreamEngine> stream_;
    std::unique_ptr<IAudioOutputDevice> device_;
    std::unique_ptr<Handle> handle_;
    mutable std::mutex control_;
    bool hptf_supported_{false};
};

// cppcheck-suppress-end unusedStructMember
} // namespace mradm::realtime
