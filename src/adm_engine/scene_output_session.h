#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "adm/errors.h"
#include "adm/options.h"

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

class SceneOutputSession {
  public:
    [[nodiscard]] static Result<std::unique_ptr<SceneOutputSession>>
    create(const std::shared_ptr<SceneStreamEngine>& stream, const SceneDeviceConfig& config);
    ~SceneOutputSession();
    SceneOutputSession(const SceneOutputSession&) = delete;
    SceneOutputSession& operator=(const SceneOutputSession&) = delete;

    void play();
    void pause();
    [[nodiscard]] Result<void> begin_epoch(std::uint64_t epoch, std::int64_t target);
    [[nodiscard]] Result<void> set_volume(float gain);
    [[nodiscard]] SceneDeviceStatus status() const;

  private:
    SceneOutputSession(std::shared_ptr<SceneStreamEngine> stream, std::unique_ptr<IAudioOutputDevice> device);
    void park();
    std::size_t pull(std::span<float> output, std::size_t frames) noexcept;

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
};

// cppcheck-suppress-end unusedStructMember
} // namespace mradm::realtime
