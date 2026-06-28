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

    // True for realtime hardware callbacks: a short pull means the user heard padding.
    // Buffered media sinks can return false because they pull ahead of playback time.
    [[nodiscard]] virtual bool pull_is_realtime_playback() const { return true; }

    // Drop any buffered-but-unplayed output (called on seek), so post-seek audio doesn't splice
    // onto stale pre-seek samples. Default no-op: realtime callback sinks hold no internal buffer
    // and pull fresh each block. The buffered media sink (AVSampleBufferAudioRenderer) overrides
    // it to clear its staging and flush the system queue.
    virtual void flush() {}

    // Halt / resume the device's own playback clock on monitor pause / resume. Default no-op:
    // realtime callback sinks stop pulling when the engine pauses (the pull returns silence), so
    // their clock is the hardware's. A buffered media sink has an independent clock that would
    // otherwise run away during a long pause — it overrides these to setRate 0 / 1.
    virtual void pause() {}
    virtual void resume() {}

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

#if defined(__APPLE__)
// macOS-only output device that enqueues the monitor's *multichannel* PCM into the system
// media playback stack (AVSampleBufferAudioRenderer) so macOS spatializes it to the headphone
// route with dynamic head tracking — instead of playing raw channels on hardware. `layout_id`
// is the project speaker layout (e.g. "4+7+0" for 7.1.4); the device resolves it to the
// CoreAudio AudioChannelLayoutTag that makes the system treat the stream as spatial content.
// Implemented in adm_apple/avsamplebuffer_device.mm — no Apple framework type crosses this
// boundary (ADR 0003). The start() channel count must match the layout's channel count.
[[nodiscard]] std::unique_ptr<IAudioOutputDevice> make_avsamplebuffer_device(std::string layout_id);
#endif

#if defined(_WIN32)
// Windows-only output device that submits the monitor's *multichannel* bed to ISpatialAudioClient as
// static spatial-audio objects, so the active system spatializer (Windows Sonic for Headphones /
// Dolby Atmos for Headphones / DTS Headphone:X) HRTF-renders it to the headphone route — instead of
// playing raw channels on hardware. `layout_id` is the project speaker layout (e.g. "4+7+0" for
// 7.1.4); the device maps each bed channel to its AudioObjectType (windows_layouts). Unlike macOS
// there is no OS-level head tracking; the bed is spatialized statically. Implemented in
// adm_windows/spatialaudioclient_device.cpp — no Windows COM type crosses this boundary (ADR 0003).
// The start() channel count must match the layout's channel count. Returns an unsupported error from
// start() when no spatial audio format is enabled on the output endpoint.
[[nodiscard]] std::unique_ptr<IAudioOutputDevice> make_spatialaudioclient_device(std::string layout_id);

// Speaker layouts the Windows system-spatial sink accepts, as plain data (no COM types) so
// adm_engine / the capabilities query can report them across the module boundary (ADR 0003). The
// macOS analog is apple_capabilities()'s non-binaural layouts. Implemented in adm_windows.
struct SystemSpatialLayoutInfo {
    std::string id;
    std::string display_name;
    uint32_t channel_count{0};
};
[[nodiscard]] std::vector<SystemSpatialLayoutInfo> system_spatial_layouts();
#endif

} // namespace mradm::realtime
