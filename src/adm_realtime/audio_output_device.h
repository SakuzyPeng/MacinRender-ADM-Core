#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "adm/errors.h"

// Abstract realtime audio output device. The monitor engine starts the device with a
// pull callback; the device's audio thread repeatedly asks the callback to fill its
// output buffer. Concrete backends (miniaudio, a test capture sink) implement this; the
// interface is deliberately third-party-free so miniaudio types never cross the module
// boundary (ADR 0003). See REALTIME_MONITORING_SLICE1.md §4.
namespace mradm::realtime {

class IAudioOutputDevice {
  public:
    virtual ~IAudioOutputDevice() = default;
    IAudioOutputDevice(const IAudioOutputDevice&) = delete;
    IAudioOutputDevice& operator=(const IAudioOutputDevice&) = delete;
    IAudioOutputDevice(IAudioOutputDevice&&) = delete;
    IAudioOutputDevice& operator=(IAudioOutputDevice&&) = delete;

    // Fill `out` (interleaved, `frames * channels` floats) with the next `frames` frames;
    // returns the number of frames produced. Fewer than requested signals end / underrun,
    // and the device pads the remainder with silence. Runs on the device's audio thread:
    // it must be wait-free (a ring drain), never block or allocate.
    using PullFn = std::function<std::size_t(std::span<float> out, std::size_t frames)>;

    // Open the device and begin pulling. `sample_rate` is the rate the engine renders at
    // (binaural: 48000); actual_sample_rate() reflects what the device actually opened
    // (may differ — the caller resamples at the boundary, or rejects).
    [[nodiscard]] virtual Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) = 0;
    virtual void stop() = 0;

    [[nodiscard]] virtual uint32_t actual_sample_rate() const = 0;

  protected:
    IAudioOutputDevice() = default;
};

// A selectable playback device, as seen by the UI. `id` is an opaque token (a serialized
// miniaudio device id, hex-encoded) round-tripped back to make_miniaudio_device to open that
// exact device; it never exposes a miniaudio type (ADR 0003). `name` is the human label.
struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool is_default{false};
};

// Enumerate the system's playback devices (default backend). Returns an empty list on
// failure (no devices / enumeration error) — callers fall back to the default device.
[[nodiscard]] std::vector<AudioDeviceInfo> enumerate_output_devices();

// Default realtime output device, backed by miniaudio. When `use_null_backend` is true it
// uses miniaudio's null backend (no hardware; the callback is driven off a timer) for
// headless / CI use. `device_id` is an opaque token from enumerate_output_devices(); empty
// (or unresolvable) opens the system default device. Implementation in miniaudio_device.cpp
// — no miniaudio type crosses this boundary (ADR 0003).
[[nodiscard]] std::unique_ptr<IAudioOutputDevice> make_miniaudio_device(bool use_null_backend = false,
                                                                        const std::string& device_id = {});

} // namespace mradm::realtime
